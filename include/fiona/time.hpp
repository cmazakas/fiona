#ifndef FIONA_TIME_HPP
#define FIONA_TIME_HPP

#include <fiona/error.hpp>      // for result, error_code
#include <fiona/io_context.hpp> // for executor

#include <fiona/detail/awaitable_base.hpp> // for awaitable_base
#include <fiona/detail/time.hpp>           // for duration_to_timespec

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <coroutine> // for coroutine_handle

#include <bits/chrono.h>      // for duration
#include <linux/time_types.h> // for __kernel_timespec

struct io_uring_cqe;

namespace fiona {

struct timer_impl {
private:
  friend struct timer_awaitable;
  friend struct timer_cancel_awaitable;
  friend struct timer;

  struct timeout_frame final : public detail::awaitable_base {
    error_code ec_;
    __kernel_timespec ts_ = { .tv_sec = 0, .tv_nsec = 0 };
    executor ex_;
    timer_impl* ptimer_ = nullptr;
    std::coroutine_handle<> h_;
    bool initiated_ = false;
    bool done_ = false;

    timeout_frame( executor ex, timer_impl* ptimer );
    ~timeout_frame();

    void reset();

    void await_process_cqe( io_uring_cqe* cqe ) override;

    std::coroutine_handle<> handle() noexcept override;

    void inc_ref() noexcept override;
    void dec_ref() noexcept override;
    int use_count() const noexcept override;
  };

  struct cancel_frame : public detail::awaitable_base {
    error_code ec_;
    executor ex_;
    timer_impl* ptimer_ = nullptr;
    std::coroutine_handle<> h_ = nullptr;
    bool initiated_ = false;
    bool done_ = false;

    cancel_frame( executor ex, timer_impl* ptimer );

    ~cancel_frame();

    void reset();

    void await_process_cqe( io_uring_cqe* cqe ) override;

    std::coroutine_handle<> handle() noexcept override;

    void inc_ref() noexcept override;
    void dec_ref() noexcept override;
    int use_count() const noexcept override;
  };

  timeout_frame tf_;
  cancel_frame cf_;
  int count_ = 0;

  friend inline void intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept;
  friend inline void intrusive_ptr_release( timer_impl* ptimer ) noexcept;

public:
  timer_impl( executor ex );
};

inline void
intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept {
  ++ptimer->count_;
}

inline void
intrusive_ptr_release( timer_impl* ptimer ) noexcept {
  --ptimer->count_;
  if ( ptimer->count_ == 0 ) {
    delete ptimer;
  }
}

struct timer_awaitable {
private:
  friend struct timer;

  boost::intrusive_ptr<timer_impl> ptimer_ = nullptr;

  timer_awaitable( boost::intrusive_ptr<timer_impl> ptimer,
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

  boost::intrusive_ptr<timer_impl> ptimer_ = nullptr;

  timer_cancel_awaitable( boost::intrusive_ptr<timer_impl> ptimer );

public:
  ~timer_cancel_awaitable();

  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<void> await_resume();
};

struct timer {
private:
  boost::intrusive_ptr<timer_impl> ptimer_ = nullptr;

public:
  timer( executor ex );

  timer( timer const& ) = delete;
  timer& operator=( timer const& ) = delete;

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
