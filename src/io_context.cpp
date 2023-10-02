#include <fiona/io_context.hpp>

#include <fiona/detail/awaitable_base.hpp>
#include <fiona/error.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <sys/mman.h>

namespace fiona {

buf_ring::buf_ring( io_uring* ring, std::size_t num_bufs, std::size_t buf_size,
                    std::uint16_t bgid )
    : bufs_( num_bufs ), ring_( ring ), bgid_{ bgid } {

  struct mmap_guard {
    void* mapped = nullptr;
    std::size_t n = 0;
    ~mmap_guard() {
      if ( mapped ) {
        munmap( mapped, n );
      }
    }
  };

  std::size_t n = sizeof( io_uring_buf ) * bufs_.size();

  void* mapped = mmap( nullptr, n, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE, 0, 0 );

  if ( mapped == MAP_FAILED ) {
    throw std::bad_alloc();
  }

  mmap_guard guard{ .mapped = mapped, .n = n };

  for ( auto& buf : bufs_ ) {
    buf.resize( buf_size );
  }

  buf_ring_ = static_cast<io_uring_buf_ring*>( mapped );
  io_uring_buf_ring_init( buf_ring_ );

  io_uring_buf_reg reg = {
      .ring_addr = reinterpret_cast<std::uintptr_t>( buf_ring_ ),
      .ring_entries = static_cast<std::uint32_t>( bufs_.size() ),
      .bgid = bgid_,
      .flags = 0,
      .resv = { 0 },
  };

  auto ret = io_uring_register_buf_ring( ring_, &reg, 0 );
  if ( ret != 0 ) {
    fiona::detail::throw_errno_as_error_code( -ret );
  }

  for ( std::size_t i = 0; i < bufs_.size(); ++i ) {
    auto& buf = bufs_[i];
    io_uring_buf_ring_add( buf_ring_, buf.data(), buf.size(), i,
                           io_uring_buf_ring_mask( bufs_.size() ), i );
  }
  io_uring_buf_ring_advance( buf_ring_, bufs_.size() );

  guard.mapped = nullptr;
}

buf_ring::~buf_ring() {
  if ( buf_ring_ ) {
    BOOST_ASSERT( ring_ );
    io_uring_unregister_buf_ring( ring_, bgid_ );
    munmap( buf_ring_, sizeof( io_uring_buf ) * bufs_.size() );
  }
}

buf_ring::buf_ring( buf_ring&& rhs ) noexcept {
  bufs_ = std::move( rhs.bufs_ );
  ring_ = rhs.ring_;
  buf_ring_ = rhs.buf_ring_;
  bgid_ = rhs.bgid_;

  rhs.ring_ = nullptr;
  rhs.buf_ring_ = nullptr;
  rhs.bgid_ = 0;
}

buf_ring&
buf_ring::operator=( buf_ring&& rhs ) noexcept {
  if ( this != &rhs ) {
    if ( buf_ring_ ) {
      BOOST_ASSERT( ring_ );
      io_uring_unregister_buf_ring( ring_, bgid_ );
      munmap( buf_ring_, sizeof( io_uring_buf ) * bufs_.size() );
    }

    bufs_ = std::move( rhs.bufs_ );
    ring_ = rhs.ring_;
    buf_ring_ = rhs.buf_ring_;
    bgid_ = rhs.bgid_;

    rhs.ring_ = nullptr;
    rhs.buf_ring_ = nullptr;
    rhs.bgid_ = 0;
  }
  return *this;
}

void
io_context::run() {
  struct cqe_guard {
    io_uring* ring;
    io_uring_cqe* cqe;

    ~cqe_guard() { io_uring_cqe_seen( ring, cqe ); }
  };

  struct advance_guard {
    io_uring* ring = nullptr;
    unsigned count = 0;
    ~advance_guard() {
      if ( ring ) {
        io_uring_cq_advance( ring, count );
      }
    }
  };

  struct guard {
    detail::task_set_type& tasks;
    io_uring* ring;

    ~guard() {
      for ( auto const& h : tasks ) {
        h.destroy();
      }

      boost::unordered_flat_set<void*> blacklist;

      io_uring_cqe* cqe = nullptr;
      while ( 0 == io_uring_peek_cqe( ring, &cqe ) ) {
        auto guard = cqe_guard( ring, cqe );
        auto p = io_uring_cqe_get_data( cqe );

        if ( p == nullptr ) {
          continue;
        }

        if ( blacklist.find( p ) != blacklist.end() ) {
          // when destructing, we can have N CQEs associated with an awaitable
          // because of multishot ops
          // our normal I/O loop handles this case fine, but during cleanup
          // like this we need to be aware that multiple CQEs can share the
          // same user_data
          continue;
        }

        auto q = boost::intrusive_ptr(
            static_cast<detail::awaitable_base*>( p ), false );

        if ( q->use_count() == 1 ) {
          blacklist.insert( p );
        }

        (void)q;
      }
    }
  };

  auto on_cqe = []( io_uring_cqe* cqe ) {
    auto p = io_uring_cqe_get_data( cqe );

    if ( !p ) {
      return;
    }

    auto q = boost::intrusive_ptr( static_cast<detail::awaitable_base*>( p ),
                                   false );
    BOOST_ASSERT( q->use_count() >= 1 );

    q->await_process_cqe( cqe );
    if ( auto h = q->handle(); h ) {
      h.resume();
    }
  };

  auto ex = get_executor();
  auto ring = detail::executor_access_policy::ring( ex );
  auto cqes = std::vector<io_uring_cqe*>( framep_->params_.cq_entries );
  auto& tasks = framep_->tasks_;

  guard g{ tasks, ring };
  while ( !tasks.empty() ) {
    if ( framep_->exception_ptr_ ) {
      std::rethrow_exception( framep_->exception_ptr_ );
    }

    auto& run_queue = framep_->run_queue_;
    while ( !run_queue.empty() ) {
      auto h = run_queue.front();
      h.resume();
      run_queue.pop_front();
    }

    if ( tasks.empty() ) {
      break;
    }

    io_uring_submit_and_wait( ring, 1 );

    auto num_ready = io_uring_cq_ready( ring );
    io_uring_peek_batch_cqe( ring, cqes.data(), num_ready );

    advance_guard guard = { .ring = ring, .count = 0 };
    for ( ; guard.count < num_ready; ) {
      on_cqe( cqes[guard.count++] );
    }
  }

  if ( framep_->exception_ptr_ ) {
    std::rethrow_exception( framep_->exception_ptr_ );
  }
}

namespace detail {

io_context_frame::io_context_frame( io_context_params const& io_ctx_params )
    : params_( io_ctx_params ) {

  int ret = -1;
  auto ring = &io_ring_;

  io_uring_params params = {};
  params.cq_entries = io_ctx_params.cq_entries;

  {
    auto& flags = params.flags;
    flags |= IORING_SETUP_CQSIZE;
    flags |= IORING_SETUP_SINGLE_ISSUER;
    flags |= IORING_SETUP_COOP_TASKRUN;
  }

  ret = io_uring_queue_init_params( params_.sq_entries, ring, &params );
  if ( ret != 0 ) {
    fiona::detail::throw_errno_as_error_code( -ret );
  }

  ret = io_uring_register_ring_fd( ring );
  if ( ret != 1 ) {
    fiona::detail::throw_errno_as_error_code( -ret );
  }

  auto const num_files = params_.num_files;
  ret = io_uring_register_files_sparse( ring, num_files );
  if ( ret != 0 ) {
    fiona::detail::throw_errno_as_error_code( -ret );
  }

  fds_.reserve( num_files );
  for ( int i = 0; i < static_cast<int>( num_files ); ++i ) {
    fds_.insert( i );
  }
}

io_context_frame::~io_context_frame() {
  auto ring = &io_ring_;
  io_uring_queue_exit( ring );
}

} // namespace detail

} // namespace fiona
