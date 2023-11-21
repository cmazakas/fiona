// clang-format off
#include <fiona/error.hpp>                         // for throw_errno_as_error_code
#include <fiona/executor.hpp>                      // for executor, executor_access_policy
#include <fiona/io_context.hpp>                    // for io_context
#include <fiona/detail/get_sqe.hpp>                // for reserve_sqes
#include <fiona/params.hpp>                        // for io_context_params
#include <fiona/task.hpp>                          // for task

#include <fiona/detail/awaitable_base.hpp>         // for intrusive_ptr_add_ref, awaitable_base, intrusive_ptr_release
#include <fiona/detail/common.hpp>                 // for buf_ring, io_context_frame, task_map_type

#include <boost/assert.hpp>                        // for BOOST_ASSERT
#include <boost/container_hash/hash.hpp>           // for hash
#include <boost/smart_ptr/intrusive_ptr.hpp>       // for intrusive_ptr
#include <boost/unordered/detail/foa/table.hpp>    // for operator!=, table_iterator
#include <boost/unordered/unordered_flat_set.hpp>  // for unordered_flat_set

#include <algorithm>                               // for copy
#include <coroutine>                               // for coroutine_handle
#include <cstdint>                                 // for uint32_t, uintptr_t, uint16_t
#include <cstring>                                 // for size_t, memcpy
#include <deque>                                   // for deque
#include <exception>                               // for rethrow_exception, exception_ptr
#include <memory>                                  // for shared_ptr, __shared_ptr_access
#include <new>                                     // for bad_alloc
#include <utility>                                 // for move
#include <vector>                                  // for vector

#include <errno.h>                                 // for errno
#include <liburing.h>                              // for io_uring_get_sqe, io_uring_sqe_set_data, io_uring_cqe_get_...
#include <mm_malloc.h>                             // for posix_memalign
#include <stdlib.h>                                // for free
#include <unistd.h>                                // for close, pipe, sysconf, _SC_PAGESIZE
#include <liburing/io_uring.h>                     // for io_uring_cqe, io_uring_params, IORING_SETUP_COOP_TASKRUN
// clang-format on

namespace {

struct pipe_awaitable {
  struct frame final : public fiona::detail::awaitable_base {
    fiona::executor ex_;
    char buffer_[sizeof( std::coroutine_handle<> )] = {};
    int fd_ = -1;
    int count_ = 0;

    frame( fiona::executor ex, int fd ) : ex_{ ex }, fd_{ fd } {}

    std::coroutine_handle<> h_ = nullptr;

    void init() {
      auto ring = fiona::detail::executor_access_policy::ring( ex_ );
      fiona::detail::reserve_sqes( ring, 1 );
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_read( sqe, fd_, buffer_, sizeof( buffer_ ), 0 );
      io_uring_sqe_set_data( sqe, this );
      intrusive_ptr_add_ref( this );
    }

    void await_process_cqe( io_uring_cqe* cqe ) override {
      if ( cqe->res != sizeof( void* ) ) {
        BOOST_ASSERT( cqe->res < 0 );
        fiona::detail::throw_errno_as_error_code( -cqe->res );
      }

      void* addr = nullptr;
      std::memcpy( &addr, buffer_, sizeof( addr ) );

      h_ = std::coroutine_handle<>::from_address( addr );

      {
        auto ring = fiona::detail::executor_access_policy::ring( ex_ );
        fiona::detail::reserve_sqes( ring, 1 );
        auto sqe = io_uring_get_sqe( ring );
        io_uring_prep_read( sqe, fd_, buffer_, sizeof( buffer_ ), 0 );
        io_uring_sqe_set_data( sqe, this );
        intrusive_ptr_add_ref( this );
      }
    }

    std::coroutine_handle<> handle() noexcept override {
      BOOST_ASSERT( h_ );
      return h_;
    }

    void inc_ref() noexcept override { ++count_; }
    void dec_ref() noexcept override {
      --count_;
      if ( count_ == 0 ) {
        delete this;
      }
    }

    int use_count() const noexcept override { return count_; }
  };

  boost::intrusive_ptr<frame> p_;

  pipe_awaitable( fiona::executor ex, int fd ) : p_( new frame( ex, fd ) ) {
    p_->init();
  }

  ~pipe_awaitable() { cancel(); }

  void cancel() {
    auto& self = *p_;
    auto ring = fiona::detail::executor_access_policy::ring( self.ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_prep_cancel( sqe, p_.get(), 0 );
    fiona::detail::submit_ring( ring );
  }
};

struct cqe_guard {
  io_uring* ring;
  io_uring_cqe* cqe;

  cqe_guard( io_uring* ring_, io_uring_cqe* cqe_ )
      : ring{ ring_ }, cqe{ cqe_ } {}
  ~cqe_guard() { io_uring_cqe_seen( ring, cqe ); }
};

struct guard {
  fiona::detail::task_map_type& tasks;
  io_uring* ring;

  ~guard() {
    while ( !tasks.empty() ) {
      auto pos = tasks.begin();
      auto [h, pcount] = *pos;
      if ( --*pcount == 0 ) {
        h.destroy();
      }
      tasks.erase( pos );
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

io_context::~io_context() {
  {
    auto ex = get_executor();
    auto ring = detail::executor_access_policy::ring( ex );
    auto& tasks = pframe_->tasks_;

    guard g{ tasks, ring };
  }
  pframe_ = nullptr;
}

executor
io_context::get_executor() const noexcept {
  return executor{ pframe_ };
}

void
io_context::post( task<void> t ) {
  auto ex = get_executor();
  ex.post( std::move( t ) );
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
  auto cqes = std::vector<io_uring_cqe*>( pframe_->params_.cq_entries );
  auto& tasks = pframe_->tasks_;

  guard g{ tasks, ring };

  {
    auto pipe_awaiter =
        pipe_awaitable( ex, detail::executor_access_policy::get_pipefd( ex ) );

    while ( !tasks.empty() ) {
      if ( pframe_->exception_ptr_ ) {
        std::rethrow_exception( pframe_->exception_ptr_ );
      }

      auto& run_queue = pframe_->run_queue_;
      while ( !run_queue.empty() ) {
        auto h = run_queue.front();
        h.resume();
        run_queue.pop_front();
        if ( pframe_->exception_ptr_ ) {
          auto p = pframe_->exception_ptr_;

          std::rethrow_exception( pframe_->exception_ptr_ );
        }
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
        if ( pframe_->exception_ptr_ ) {
          auto p = pframe_->exception_ptr_;

          std::rethrow_exception( pframe_->exception_ptr_ );
        }
      }
    }

    if ( pframe_->exception_ptr_ ) {
      std::rethrow_exception( pframe_->exception_ptr_ );
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
    flags |= IORING_SETUP_TASKRUN_FLAG;
    flags |= IORING_SETUP_DEFER_TASKRUN;
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
  close( pipefd_[0] );
  close( pipefd_[1] );

  auto ring = &io_ring_;
  io_uring_queue_exit( ring );
}

} // namespace detail

} // namespace fiona
