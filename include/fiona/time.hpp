#ifndef FIONA_TIME_HPP
#define FIONA_TIME_HPP

#include <fiona/error.hpp>                   // for result
#include <fiona/executor.hpp>                // for executor

#include <fiona/detail/config.hpp>           // for FIONA_DECL
#include <fiona/detail/time.hpp>             // for duration_to_timespec

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <chrono>                            // for duration
#include <coroutine>                         // for coroutine_handle

namespace fiona {
namespace detail {
struct timer_impl;

void FIONA_DECL
intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept;

void FIONA_DECL
intrusive_ptr_release( timer_impl* ptimer ) noexcept;
} // namespace detail
} // namespace fiona

struct __kernel_timespec;

namespace fiona {

struct timer_awaitable {
private:
  friend struct timer;

  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

  FIONA_DECL
  timer_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer, __kernel_timespec ts );

public:
  FIONA_DECL
  ~timer_awaitable();

  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<void> await_resume();
};

struct timer_cancel_awaitable {
private:
  friend struct timer;

  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

  timer_cancel_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer );

public:
  FIONA_DECL
  ~timer_cancel_awaitable();

  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<void> await_resume();
};

struct timer {
private:
  boost::intrusive_ptr<detail::timer_impl> ptimer_ = nullptr;

public:
  FIONA_DECL
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

  FIONA_DECL
  timer_cancel_awaitable async_cancel();

  FIONA_DECL
  executor get_executor() const noexcept;
};

} // namespace fiona

#endif // FIONA_TIME_HPP
