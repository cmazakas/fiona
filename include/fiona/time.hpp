// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_TIME_HPP
#define FIONA_TIME_HPP

#include <fiona/error.hpp>                   // for result
#include <fiona/executor.hpp>                // for executor

#include <fiona/detail/time.hpp>             // for duration_to_timespec

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <chrono>                            // for duration
#include <coroutine>                         // for coroutine_handle

#include <fiona_export.h>

namespace fiona {
namespace detail {

struct timer_impl;

void FIONA_EXPORT intrusive_ptr_add_ref( timer_impl* );

void FIONA_EXPORT intrusive_ptr_release( timer_impl* );

} // namespace detail
} // namespace fiona

struct __kernel_timespec;

namespace fiona {

struct timer_awaitable
{
private:
  friend struct timer;

  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

  FIONA_EXPORT
  timer_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer,
                   __kernel_timespec ts );

public:
  FIONA_EXPORT
  ~timer_awaitable();

  FIONA_EXPORT
  bool await_ready() const;

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<void> await_resume();
};

struct timer_cancel_awaitable
{
private:
  friend struct timer;

  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

  timer_cancel_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer );

public:
  FIONA_EXPORT
  ~timer_cancel_awaitable();

  FIONA_EXPORT
  bool await_ready() const;

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<void> await_resume();
};

struct timer
{
private:
  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

public:
  FIONA_EXPORT
  timer( executor ex );

  FIONA_EXPORT
  timer( timer const& ) = default;
  FIONA_EXPORT
  timer& operator=( timer const& ) = default;

  FIONA_EXPORT
  timer( timer&& ) = default;
  FIONA_EXPORT
  timer& operator=( timer&& ) = default;

  FIONA_EXPORT
  ~timer() = default;

  template <class Rep, class Period>
  timer_awaitable
  async_wait( std::chrono::duration<Rep, Period> d )
  {
    auto ts = detail::duration_to_timespec( d );
    return { ptimer_, ts };
  }

  FIONA_EXPORT
  timer_cancel_awaitable async_cancel();

  FIONA_EXPORT
  executor get_executor() const noexcept;
};

} // namespace fiona

#endif // FIONA_TIME_HPP
