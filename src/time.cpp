// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <fiona/time.hpp>

#include <fiona/error.hpp>                   // for error_code, result, thr...
#include <fiona/executor.hpp>                // for executor, executor_acce...

#include <fiona/detail/get_sqe.hpp>          // for get_sqe, submit_ring

#include <boost/assert.hpp>                  // for BOOST_ASSERT
#include <boost/config.hpp>                  // for BOOST_NOINLINE, BOOST_N...
#include <boost/core/exchange.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <coroutine>                         // for coroutine_handle
#include <system_error>                      // for make_error_code, errc
#include <utility>                           // for move

#include <errno.h>                           // for EBUSY, ETIME
#include <liburing.h>                        // for io_uring_sqe_set_data
#include <liburing/io_uring.h>               // for io_uring_cqe
#include <linux/time_types.h>                // for __kernel_timespec

#include "awaitable_base.hpp"                // for awaitable_base, intrusi...

namespace fiona {
namespace {

BOOST_NOINLINE BOOST_NORETURN inline void
throw_busy()
{
  detail::throw_errno_as_error_code( EBUSY );
}

} // namespace

namespace detail {
namespace {

struct timeout_frame : public detail::awaitable_base
{
  timer_impl* ptimer_ = nullptr;
  error_code ec_;
  __kernel_timespec ts_ = { .tv_sec = 0, .tv_nsec = 0 };
  std::coroutine_handle<> h_;
  bool initiated_ = false;
  bool done_ = false;

  timeout_frame( timer_impl* ptimer ) : ptimer_( ptimer ) {}

  virtual ~timeout_frame() override;

  void
  reset()
  {
    ts_ = { .tv_sec = 0, .tv_nsec = 0 };
    h_ = nullptr;
    initiated_ = false;
    done_ = false;
    ec_ = {};
  }

  void
  await_process_cqe( io_uring_cqe* cqe ) override
  {
    done_ = true;
    auto e = -cqe->res;
    if ( e != 0 && e != ETIME ) {
      ec_ = error_code::from_errno( e );
    }
  }

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }
};

struct cancel_frame : public detail::awaitable_base
{
  timer_impl* ptimer_ = nullptr;
  error_code ec_;
  std::coroutine_handle<> h_ = nullptr;
  bool initiated_ = false;
  bool done_ = false;

  cancel_frame( timer_impl* ptimer ) : ptimer_( ptimer ) {}
  virtual ~cancel_frame() override;

  void
  reset()
  {
    initiated_ = false;
    done_ = false;
    ec_ = {};
  }

  void
  await_process_cqe( io_uring_cqe* cqe ) override
  {
    done_ = true;
    if ( cqe->res < 0 ) {
      ec_ = error_code::from_errno( -cqe->res );
    }
  }

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }
};

} // namespace

struct timer_impl : public virtual ref_count,
                    public timeout_frame,
                    public cancel_frame
{
private:
  friend struct fiona::timer_awaitable;
  friend struct fiona::timer_cancel_awaitable;
  friend struct fiona::timer;

  executor ex_;

public:
  timer_impl( executor ex )
      : timeout_frame( this ), cancel_frame( this ), ex_( ex )
  {
  }

  virtual ~timer_impl() override;
};

void
intrusive_ptr_add_ref( timer_impl* ptimer )
{
  intrusive_ptr_add_ref( static_cast<ref_count*>( ptimer ) );
}

void
intrusive_ptr_release( timer_impl* ptimer )
{
  intrusive_ptr_release( static_cast<ref_count*>( ptimer ) );
}

timeout_frame::~timeout_frame() {}
cancel_frame::~cancel_frame() {}
timer_impl::~timer_impl() {}

} // namespace detail

timer_awaitable::timer_awaitable(
    boost::intrusive_ptr<detail::timer_impl> ptimer, __kernel_timespec ts )
    : ptimer_( ptimer )
{
  ptimer_->timeout_frame::ts_ = ts;
}

timer_awaitable::~timer_awaitable()
{
  auto& tf = static_cast<detail::timeout_frame&>( *ptimer_ );
  if ( tf.initiated_ && !tf.done_ ) {
    auto ring = detail::executor_access_policy::ring( ptimer_->ex_ );

    auto sqe = detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, &tf, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    fiona::detail::submit_ring( ring );
  }
}

bool
timer_awaitable::await_ready() const
{
  if ( ptimer_->timeout_frame::initiated_ ) {
    throw_busy();
  }
  return false;
}

void
timer_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto& tf = static_cast<detail::timeout_frame&>( *ptimer_ );
  if ( tf.initiated_ ) {
    throw_busy();
  }

  tf.h_ = h;

  auto ring = detail::executor_access_policy::ring( ptimer_->ex_ );
  auto sqe = detail::get_sqe( ring );

  io_uring_prep_timeout( sqe, &tf.ts_, 0, 0 );
  io_uring_sqe_set_data(
      sqe, boost::intrusive_ptr<detail::awaitable_base>( &tf ).detach() );

  tf.initiated_ = true;
}

result<void>
timer_awaitable::await_resume()
{
  auto& tf = static_cast<detail::timeout_frame&>( *ptimer_ );
  auto ec = std::move( tf.ec_ );
  tf.reset();
  if ( ec ) {
    return { ec };
  }
  return {};
}

timer_cancel_awaitable::timer_cancel_awaitable(
    boost::intrusive_ptr<detail::timer_impl> ptimer )
    : ptimer_( ptimer )
{
}

timer_cancel_awaitable::~timer_cancel_awaitable() {}

bool
timer_cancel_awaitable::await_ready() const
{
  if ( ptimer_->cancel_frame::initiated_ ) {
    throw_busy();
  }
  return false;
}

void
timer_cancel_awaitable::await_suspend( std::coroutine_handle<> h )
{
  if ( ptimer_->cancel_frame::initiated_ ) {
    throw_busy();
  }

  auto& cf = static_cast<detail::cancel_frame&>( *ptimer_ );

  auto ring = detail::executor_access_policy::ring( ptimer_->ex_ );
  auto sqe = detail::get_sqe( ring );
  io_uring_prep_cancel(
      sqe, static_cast<detail::timeout_frame*>( ptimer_.get() ), 0 );
  io_uring_sqe_set_data(
      sqe, boost::intrusive_ptr<detail::awaitable_base>( &cf ).detach() );

  cf.h_ = h;
  cf.initiated_ = true;
}

result<void>
timer_cancel_awaitable::await_resume()
{
  auto& cf = static_cast<detail::cancel_frame&>( *ptimer_ );
  if ( !cf.initiated_ || !cf.done_ ) {
    throw_busy();
  }

  auto ec = std::move( cf.ec_ );
  cf.reset();
  if ( ec ) {
    return { ec };
  }
  return {};
}

timer::timer( executor ex ) : ptimer_{ new detail::timer_impl( ex ) } {}

timer_cancel_awaitable
timer::async_cancel()
{
  return { ptimer_ };
}

executor
timer::get_executor() const noexcept
{
  return ptimer_->ex_;
}

} // namespace fiona
