// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <fiona/io_context.hpp>

#include <fiona/detail/common.hpp>                // for io_context_frame
#include <fiona/detail/get_sqe.hpp>               // for reserve_sqes, subm...
#include <fiona/error.hpp>                        // for throw_errno_as_err...
#include <fiona/executor.hpp>                     // for executor_access_po...
#include <fiona/params.hpp>                       // for io_context_params
#include <fiona/task.hpp>                         // for task

#include <boost/assert.hpp>                       // for BOOST_ASSERT
#include <boost/core/exchange.hpp>                // for exchange
#include <boost/smart_ptr/intrusive_ptr.hpp>      // for intrusive_ptr
#include <boost/unordered/unordered_flat_set.hpp> // for unordered_flat_set

#include <algorithm>                              // for copy
#include <array>                                  // for array
#include <cerrno>
#include <coroutine>                              // for coroutine_handle
#include <cstring>                                // for memcpy, size_t
#include <deque>                                  // for deque
#include <exception>                              // for exception_ptr, ret...
#include <memory>                                 // for shared_ptr, __shar...
#include <utility>                                // for move, pair
#include <vector>                                 // for vector

#include <errno.h>                                // for errno
#include <liburing.h>                             // for io_uring_cqe_get_data
#include <liburing/io_uring.h>                    // for io_uring_cqe, io_u...
#include <unistd.h>                               // for close, pipe

#include "detail/awaitable_base.hpp"              // for awaitable_base

namespace {

struct frame : public fiona::detail::awaitable_base
{
  fiona::executor ex_;
  std::coroutine_handle<> h_ = nullptr;
  alignas( std::coroutine_handle<> ) unsigned char buffer_[sizeof(
      std::coroutine_handle<> )] = {};
  int fd_ = -1;

  frame( fiona::executor ex, int fd ) : ex_{ ex }, fd_{ fd } {}

  void
  schedule_recv()
  {
    auto ring = fiona::detail::executor_access_policy::ring( ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_read( sqe, fd_, buffer_, sizeof( buffer_ ), 0 );
    io_uring_sqe_set_data( sqe, this );
    intrusive_ptr_add_ref( this );
  }

  void
  await_process_cqe( io_uring_cqe* cqe ) override
  {
    if ( cqe->res != sizeof( void* ) ) {
      BOOST_ASSERT( cqe->res < 0 );
      fiona::detail::throw_errno_as_error_code( -cqe->res );
    }

    std::uintptr_t data = 0;
    std::memcpy( &data, buffer_, sizeof( data ) );

    if ( data & fiona::detail::wake_mask ) {
      data &= fiona::detail::ptr_mask;
      h_ = std::coroutine_handle<>::from_address(
          reinterpret_cast<void*>( data ) );

    } else if ( data & fiona::detail::post_mask ) {
      // TODO: determine if tsan is just giving us false positives here and if
      // we should remove lock/unlocking the mutex here
      {
        auto guard = fiona::detail::executor_access_policy::lock_guard( ex_ );
      }

      data &= fiona::detail::ptr_mask;

      auto& tasks = fiona::detail::executor_access_policy::tasks( ex_ );
      auto& run_queue = fiona::detail::executor_access_policy::run_queue( ex_ );

      auto task =
          fiona::task<void>::from_address( reinterpret_cast<void*>( data ) );
      auto internal_task = fiona::detail::scheduler( tasks, std::move( task ) );
      tasks.add_task( internal_task.h_ );
      run_queue.push_back( internal_task.h_ );
    }

    schedule_recv();
  }

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }
};

struct pipe_awaitable
{
  boost::intrusive_ptr<frame> p_;

  pipe_awaitable( fiona::executor ex, int fd ) : p_( new frame( ex, fd ) )
  {
    p_->schedule_recv();
  }

  ~pipe_awaitable() { cancel(); }

  void
  cancel()
  {
    auto& self = *p_;
    auto ring = fiona::detail::executor_access_policy::ring( self.ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_prep_cancel( sqe, p_.get(), 0 );
    fiona::detail::submit_ring( ring );
  }
};

struct cqe_guard
{
  io_uring* ring;
  io_uring_cqe* cqe;

  cqe_guard( io_uring* ring_, io_uring_cqe* cqe_ ) : ring{ ring_ }, cqe{ cqe_ }
  {
  }
  ~cqe_guard() { io_uring_cqe_seen( ring, cqe ); }
};

struct guard
{
  fiona::detail::task_map& tasks;
  io_uring* ring;

  ~guard()
  {
    io_uring_submit( ring );

    tasks.clear();

    boost::unordered_flat_set<void*> blacklist;

    io_uring_cqe* cqe = nullptr;
    while ( 0 == io_uring_peek_cqe( ring, &cqe ) ) {
      auto guard = cqe_guard( ring, cqe );
      auto p = io_uring_cqe_get_data( cqe );

      if ( p == nullptr ) {
        continue;
      }

      if ( blacklist.contains( p ) ) {
        // when destructing, we can have N CQEs associated with an awaitable
        // because of multishot ops
        // our normal I/O loop handles this case fine, but during cleanup
        // like this we need to be aware that multiple CQEs can share the
        // same user_data
        continue;
      }

      blacklist.insert( p );

      intrusive_ptr_release( static_cast<fiona::detail::awaitable_base*>( p ) );
    }
  }
};
} // namespace

namespace fiona {

io_context::~io_context()
{
  {
    auto ex = get_executor();
    auto ring = detail::executor_access_policy::ring( ex );
    auto& tasks = pframe_->tasks_;

    guard g{ tasks, ring };
  }
  pframe_ = nullptr;
}

executor
io_context::get_executor() const noexcept
{
  return executor{ pframe_ };
}

void
io_context::run()
{
  struct advance_guard
  {
    io_uring* ring = nullptr;
    unsigned count = 0;
    ~advance_guard()
    {
      if ( ring ) {
        io_uring_cq_advance( ring, count );
      }
    }
  };

  auto ex = get_executor();
  auto ring = detail::executor_access_policy::ring( ex );
  auto cqes = std::vector<io_uring_cqe*>( pframe_->params_.cq_entries );
  auto& tasks = pframe_->tasks_;

  guard g{ tasks, ring };
  {
    pipe_awaitable pipe_awaiter(
        ex, detail::executor_access_policy::get_pipefd( ex ) );

    std::vector<std::coroutine_handle<>> handles( pframe_->params_.cq_entries );

    while ( !tasks.empty() ) {
      if ( BOOST_UNLIKELY( static_cast<bool>( pframe_->exception_ptr_ ) ) ) {
        auto p = pframe_->exception_ptr_;
        std::rethrow_exception( pframe_->exception_ptr_ );
      }

      auto& run_queue = pframe_->run_queue_;
      while ( !run_queue.empty() ) {
        auto h = run_queue.front();
        h.resume();
        run_queue.pop_front();

        if ( BOOST_UNLIKELY( static_cast<bool>( pframe_->exception_ptr_ ) ) ) {
          auto p = pframe_->exception_ptr_;
          std::rethrow_exception( pframe_->exception_ptr_ );
        }
      }

      if ( tasks.empty() ) {
        // if tasks are empty and we don't break here, we get stuck waiting
        // forever for I/O that'll never come
        break;
      }

      io_uring_submit_and_wait( ring, 1 );

      auto num_ready = io_uring_cq_ready( ring );
      io_uring_peek_batch_cqe( ring, cqes.data(), num_ready );

      auto phandles = handles.begin();

      {
        advance_guard cqe_guard{ ring, 0 };
        for ( ; cqe_guard.count < num_ready; ++cqe_guard.count ) {
          auto cqe = cqes[cqe_guard.count];
          auto p = io_uring_cqe_get_data( cqe );
          if ( !p ) {
            continue;
          }

          boost::intrusive_ptr<detail::awaitable_base> q(
              static_cast<detail::awaitable_base*>( p ), false );
          BOOST_ASSERT( q->use_count() >= 1 );

          q->await_process_cqe( cqe );
          if ( auto h = q->handle(); h ) {
            *phandles++ = h;
          }

          if ( BOOST_UNLIKELY(
                   static_cast<bool>( pframe_->exception_ptr_ ) ) ) {
            auto p = pframe_->exception_ptr_;
            std::rethrow_exception( pframe_->exception_ptr_ );
          }
        }
      }

      for ( auto pos = handles.begin(); pos < phandles; ++pos ) {
        pos->resume();
      }
    }

    if ( BOOST_UNLIKELY( static_cast<bool>( pframe_->exception_ptr_ ) ) ) {
      auto p = pframe_->exception_ptr_;
      std::rethrow_exception( pframe_->exception_ptr_ );
    }
  }
}

namespace detail {

io_context_frame::io_context_frame( io_context_params const& io_ctx_params )
    : params_( io_ctx_params )
{

  int ret = -1;
  auto ring = &io_ring_;

  ret = pipe( pipefd_.data() );
  if ( ret == -1 ) {
    detail::throw_errno_as_error_code( errno );
  }

  io_uring_params params = {};
  params.cq_entries = io_ctx_params.cq_entries;

  {
    auto& flags = params.flags;
    flags |= IORING_SETUP_CQSIZE;
    flags |= IORING_SETUP_SINGLE_ISSUER;
    // flags |= IORING_SETUP_COOP_TASKRUN;
    // flags |= IORING_SETUP_TASKRUN_FLAG;
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

io_context_frame::~io_context_frame()
{
  close( pipefd_[0] );
  close( pipefd_[1] );

  auto ring = &io_ring_;
  io_uring_queue_exit( ring );
}

} // namespace detail

} // namespace fiona
