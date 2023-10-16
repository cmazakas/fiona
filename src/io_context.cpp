#include <fiona/io_context.hpp>

#include <fiona/detail/awaitable_base.hpp>
#include <fiona/detail/get_sqe.hpp>
#include <fiona/error.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <stdlib.h>
#include <unistd.h>

namespace {

struct cqe_guard {
  io_uring* ring;
  io_uring_cqe* cqe;

  ~cqe_guard() { io_uring_cqe_seen( ring, cqe ); }
};

struct guard {
  fiona::detail::task_set_type& tasks;
  io_uring* ring;

  ~guard() {
    for ( auto const& h : tasks ) {
      h.destroy();
    }
    tasks.clear();

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
          static_cast<fiona::detail::awaitable_base*>( p ), false );

      if ( q->use_count() == 1 ) {
        blacklist.insert( p );
      }

      (void)q;
    }
  }
};
} // namespace

namespace fiona {

buf_ring::buf_ring( io_uring* ring, std::size_t num_bufs, std::size_t buf_size,
                    std::uint16_t bgid )
    : bufs_( num_bufs ), ring_( ring ), bgid_{ bgid } {

  struct posix_memalign_guard {
    void* p = nullptr;
    ~posix_memalign_guard() {
      if ( p ) {
        free( p );
      }
    }
  };

  int ret = -1;

  posix_memalign_guard guard;

  std::size_t n = sizeof( io_uring_buf ) * bufs_.size();
  std::size_t pagesize = sysconf( _SC_PAGESIZE );
  ret = posix_memalign( &guard.p, pagesize, n );
  if ( ret != 0 ) {
    throw std::bad_alloc();
  }

  for ( auto& buf : bufs_ ) {
    buf.resize( buf_size );
  }

  buf_ring_ = static_cast<io_uring_buf_ring*>( guard.p );
  io_uring_buf_ring_init( buf_ring_ );

  io_uring_buf_reg reg = {
      .ring_addr = reinterpret_cast<std::uintptr_t>( buf_ring_ ),
      .ring_entries = static_cast<std::uint32_t>( bufs_.size() ),
      .bgid = bgid_,
      .flags = 0,
      .resv = { 0 },
  };

  ret = io_uring_register_buf_ring( ring_, &reg, 0 );
  if ( ret != 0 ) {
    fiona::detail::throw_errno_as_error_code( -ret );
  }

  for ( std::size_t i = 0; i < bufs_.size(); ++i ) {
    auto& buf = bufs_[i];
    io_uring_buf_ring_add( buf_ring_, buf.data(), buf.size(), i,
                           io_uring_buf_ring_mask( bufs_.size() ), i );
  }
  io_uring_buf_ring_advance( buf_ring_, bufs_.size() );

  guard.p = nullptr;
}

buf_ring::~buf_ring() {
  if ( buf_ring_ ) {
    BOOST_ASSERT( ring_ );
    io_uring_unregister_buf_ring( ring_, bgid_ );
    free( buf_ring_ );
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
      free( buf_ring_ );
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

io_context::~io_context() {
  {
    auto ex = get_executor();
    auto ring = detail::executor_access_policy::ring( ex );
    auto& tasks = framep_->tasks_;

    guard g{ tasks, ring };
  }
  framep_ = nullptr;
}

void
io_context::run() {
  struct advance_guard {
    io_uring* ring = nullptr;
    unsigned count = 0;
    ~advance_guard() {
      if ( ring ) {
        io_uring_cq_advance( ring, count );
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

  {
    auto pipe_awaiter =
        detail::executor_access_policy::get_pipe_awaitable( ex );

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
}

namespace detail {

io_context_frame::io_context_frame( io_context_params const& io_ctx_params )
    : params_( io_ctx_params ) {

  int ret = -1;
  auto ring = &io_ring_;

  ret = pipe( pipefd_ );
  if ( ret == -1 ) {
    detail::throw_errno_as_error_code( errno );
  }

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
