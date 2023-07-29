#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

#include <fiona/detail/awaitable_base.hpp>
#include <fiona/task.hpp>

#include <boost/assert.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <coroutine>
#include <cstddef>
#include <iostream>
#include <span>

#include <liburing.h>
#include <sys/mman.h>

namespace fiona {

namespace detail {
struct io_context_frame;
}

using buffer_sequence_type = std::vector<std::vector<unsigned char>>;

struct io_context_params {
  std::uint32_t sq_entries = 512;
  std::uint32_t cq_entries = 4096;
  std::uint32_t num_files_ = 1024;
};

struct buf_ring {
private:
  buffer_sequence_type bufs_;
  io_uring* ring_ = nullptr;
  io_uring_buf_ring* buf_ring_ = nullptr;
  std::uint16_t bgid_ = 0;

public:
  buf_ring() = delete;

  buf_ring( buf_ring const& ) = delete;
  buf_ring& operator=( buf_ring const& ) = delete;

  buf_ring( io_uring* ring, std::size_t num_bufs, std::size_t buf_size,
            std::uint16_t bgid )
      : bufs_( num_bufs ), ring_( ring ), bgid_{ bgid } {

    void* mapped =
        mmap( nullptr, sizeof( io_uring_buf ) * bufs_.size(),
              PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0 );

    if ( mapped == MAP_FAILED ) {
      throw std::bad_alloc();
    }

    for ( auto& buf : bufs_ ) {
      buf.resize( buf_size );
    }

    buf_ring_ = static_cast<io_uring_buf_ring*>( mapped );

    io_uring_buf_reg reg = {
        .ring_addr = reinterpret_cast<std::uintptr_t>( buf_ring_ ),
        .ring_entries = static_cast<std::uint32_t>( bufs_.size() ),
        .bgid = bgid_,
        .flags = 0,
        .resv = { 0 },
    };

    io_uring_buf_ring_init( buf_ring_ );

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
  }

  ~buf_ring() {
    if ( buf_ring_ ) {
      BOOST_ASSERT( ring_ );
      io_uring_unregister_buf_ring( ring_, bgid_ );
      munmap( buf_ring_, sizeof( io_uring_buf ) * bufs_.size() );
    }
  }

  buf_ring( buf_ring&& rhs ) noexcept {
    bufs_ = std::move( rhs.bufs_ );
    ring_ = rhs.ring_;
    buf_ring_ = rhs.buf_ring_;
    bgid_ = rhs.bgid_;

    rhs.ring_ = nullptr;
    rhs.buf_ring_ = nullptr;
    rhs.bgid_ = 0;
  }

  buf_ring& operator=( buf_ring&& rhs ) noexcept {
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

  std::span<unsigned char> get_buffer( std::size_t bid ) noexcept {
    return { bufs_[bid] };
  }

  io_uring_buf_ring* get() const noexcept { return buf_ring_; }

  std::size_t size() const noexcept { return bufs_.size(); }

  std::uint16_t bgid() const noexcept { return bgid_; }
};

struct executor {
private:
  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  executor( boost::local_shared_ptr<detail::io_context_frame> framep ) noexcept
      : framep_( std::move( framep ) ) {}

  io_uring* ring() const noexcept;
  void post( task<void> t );
  buf_ring* get_buffer_group( std::size_t bgid ) noexcept;
};

namespace detail {

struct internal_promise;

struct internal_task {
  using promise_type = internal_promise;

  std::coroutine_handle<promise_type> h_;

  internal_task( std::coroutine_handle<promise_type> h ) : h_( h ) {}
  ~internal_task() {
    if ( h_ ) {
      h_.destroy();
    }
  }

  internal_task( internal_task const& ) = delete;
  internal_task& operator=( internal_task const& ) = delete;

  internal_task( internal_task&& rhs ) noexcept : h_( rhs.h_ ) {
    rhs.h_ = nullptr;
  }
  internal_task& operator=( internal_task&& rhs ) noexcept {
    if ( this != &rhs ) {
      auto h = h_;
      h_ = rhs.h_;
      rhs.h_ = h;
    }
    return *this;
  }

  friend std::size_t hash_value( internal_task const& t ) noexcept {
    boost::hash<void*> hasher;
    return hasher( t.h_.address() );
  }

  friend bool operator==( internal_task const& lhs, internal_task const& rhs ) {
    return lhs.h_.address() == rhs.h_.address();
  }
};

struct hasher {
  using is_transparent = void;

  template <class T>
  std::size_t operator()( T const& t ) const noexcept {
    boost::hash<T> hasher;
    return hasher( t );
  }
};

struct key_equal {
  using is_transparent = void;

  template <class T, class U>
  bool operator()( T const& t, U const& u ) const {
    return t == u;
  }

  bool operator()( internal_task const& t, void* p ) const noexcept {
    return t.h_.address() == p;
  }

  bool operator()( void* p, internal_task const& t ) const noexcept {
    return t.h_.address() == p;
  }
};

using task_set_type =
    boost::unordered_flat_set<internal_task, hasher, key_equal>;

struct internal_promise {
  struct internal_final_awaitable {
    task_set_type& tasks;

    bool await_ready() const noexcept { return false; }
    void await_suspend( std::coroutine_handle<> h ) noexcept {
      tasks.erase( h.address() );
    }

    void await_resume() noexcept { BOOST_ASSERT( false ); }
  };

  template <class... Args>
  internal_promise( task_set_type& tasks_, Args&&... ) : tasks( tasks_ ) {}

  internal_task get_return_object() {
    return internal_task(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  std::suspend_always initial_suspend() { return {}; }
  internal_final_awaitable final_suspend() noexcept { return { tasks }; }

  void unhandled_exception() {
    std::rethrow_exception( std::current_exception() );
  }

  void return_void() {}

  task_set_type& tasks;
};

struct io_context_frame {
  io_uring io_ring_;
  task_set_type tasks_;
  io_context_params params_;
  std::vector<buf_ring> buf_rings_;

  io_context_frame( io_context_params const& io_ctx_params )
      : params_( io_ctx_params ) {

    int ret = -1;

    io_uring_params params = {};
    params.cq_entries = io_ctx_params.cq_entries;

    {
      auto& flags = params.flags;
      flags |= IORING_SETUP_CQSIZE;
      flags |= IORING_SETUP_SINGLE_ISSUER;
      flags |= IORING_SETUP_COOP_TASKRUN;
    }

    ret = io_uring_queue_init_params( params_.sq_entries, &io_ring_, &params );
    if ( ret != 0 ) {
      fiona::detail::throw_errno_as_error_code( -ret );
    }

    ret = io_uring_register_ring_fd( &io_ring_ );
    if ( ret != 1 ) {
      fiona::detail::throw_errno_as_error_code( -ret );
    }

    ret = io_uring_register_files_sparse( &io_ring_, params_.num_files_ );
    if ( ret != 0 ) {
      fiona::detail::throw_errno_as_error_code( -ret );
    }
  }

  ~io_context_frame() { io_uring_queue_exit( &io_ring_ ); }
};

} // namespace detail

io_uring*
executor::ring() const noexcept {
  return &framep_->io_ring_;
}

detail::internal_task
scheduler( detail::task_set_type& /* tasks */, io_uring* /* ring */,
           task<void> t ) {
  co_await t;
  co_return;
}

void
executor::post( task<void> t ) {
  auto it = scheduler( framep_->tasks_, ring(), std::move( t ) );

  auto sqe = io_uring_get_sqe( ring() );
  io_uring_prep_nop( sqe );
  io_uring_sqe_set_data( sqe, it.h_.address() );
  io_uring_submit( ring() );

  framep_->tasks_.insert( std::move( it ) );
}

buf_ring*
executor::get_buffer_group( std::size_t bgid ) noexcept {
  for ( auto& bg : framep_->buf_rings_ ) {
    if ( bgid == bg.bgid() ) {
      return &bg;
    }
  }
  return nullptr;
}

struct io_context {
private:
  struct cqe_guard {
    io_uring* ring;
    io_uring_cqe* cqe;

    ~cqe_guard() { io_uring_cqe_seen( ring, cqe ); }
  };

  decltype( auto ) tasks() const noexcept { return ( framep_->tasks_ ); }

private:
  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  io_context( io_context_params const& params = {} )
      : framep_(
            boost::make_local_shared<detail::io_context_frame>( params ) ) {}

  executor get_executor() const noexcept { return executor{ framep_ }; }
  io_context_params params() const noexcept { return framep_->params_; }

  void post( task<void> t ) {
    auto ex = get_executor();
    ex.post( std::move( t ) );
  }

  void register_buffer_sequence( std::size_t num_bufs, std::size_t buf_size,
                                 std::uint16_t buffer_group_id ) {
    auto ring = &framep_->io_ring_;
    auto br = buf_ring( ring, num_bufs, buf_size, buffer_group_id );
    framep_->buf_rings_.push_back( std::move( br ) );
  }

  void run() {
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
        boost::unordered_flat_set<void*> task_addrs;
        for ( auto const& t : tasks ) {
          task_addrs.insert( t.h_.address() );
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

          if ( task_addrs.find( p ) != task_addrs.end() ) {
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

    auto on_cqe = [this]( io_uring_cqe* cqe ) {
      auto p = io_uring_cqe_get_data( cqe );

      if ( !p ) {
        return;
      }

      if ( auto pos = tasks().find( p ); pos != tasks().end() ) {
        auto h = std::coroutine_handle<>::from_address( p );

        BOOST_ASSERT( !h.done() );
        h.resume();

        return;
      }

      auto q = boost::intrusive_ptr( static_cast<detail::awaitable_base*>( p ),
                                     false );
      BOOST_ASSERT( q->use_count() >= 1 );

      if ( q->use_count() == 1 ) {
        return;
      }

      q->await_process_cqe( cqe );
      if ( auto h = q->handle(); h ) {
        h.resume();
      }
    };

    auto ex = get_executor();
    auto ring = ex.ring();
    auto cqes = std::vector<io_uring_cqe*>( framep_->params_.cq_entries );

    guard g{ tasks(), ring };
    while ( !tasks().empty() ) {
      auto num_ready = io_uring_cq_ready( ring );
      if ( num_ready > 0 ) {
        io_uring_peek_batch_cqe( ring, cqes.data(), num_ready );

        advance_guard guard = { .ring = ring, .count = 0 };
        for ( ; guard.count < num_ready; ) {
          on_cqe( cqes[guard.count++] );
        }
        continue;
      }

      io_uring_cqe* cqe = nullptr;
      io_uring_wait_cqe( ring, &cqe );
      auto guard = cqe_guard( ring, cqe );
      on_cqe( cqe );
    }
  }
};

} // namespace fiona

#endif // FIONA_IO_CONTEXT_HPP
