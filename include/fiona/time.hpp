#ifndef FIONA_TIME_HPP
#define FIONA_TIME_HPP

#include <fiona/error.hpp>      // for result
#include <fiona/io_context.hpp> // for executor

#include <fiona/detail/time.hpp> // for duration_to_timespec

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <chrono>    // for duration
#include <coroutine> // for coroutine_handle

#include <linux/time_types.h> // for __kernel_timespec

namespace fiona {

namespace detail {
struct timer_impl;

void
intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept;

void
intrusive_ptr_release( timer_impl* ptimer ) noexcept;
} // namespace detail

struct timer_awaitable {
private:
  friend struct timer;

  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

  timer_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer,
                   __kernel_timespec ts );

public:
  ~timer_awaitable();

  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<void> await_resume();
};

struct timer_cancel_awaitable {
private:
  friend struct timer;

  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

  timer_cancel_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer );

public:
  ~timer_cancel_awaitable();

  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<void> await_resume();
};

struct timer {
private:
  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

public:
  timer( executor ex );

  timer( timer const& ) = default;
  timer& operator=( timer const& ) = default;

  timer( timer&& ) = default;
  timer& operator=( timer&& ) = default;

  ~timer() = default;

  template <class Rep, class Period>
  timer_awaitable async_wait( std::chrono::duration<Rep, Period> d ) {
    auto ts = detail::duration_to_timespec( d );
    return { ptimer_, ts };
  }

  timer_cancel_awaitable async_cancel();

  executor get_executor() const noexcept;
};

} // namespace fiona

#endif // FIONA_TIME_HPP
