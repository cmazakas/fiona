#ifndef FIONA_TIME_HPP
#define FIONA_TIME_HPP

#include <fiona/error.hpp>
#include <fiona/io_context.hpp>

#include <fiona/detail/awaitable_base.hpp>
#include <fiona/detail/get_sqe.hpp>
#include <fiona/detail/time.hpp>

#include <boost/config.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <chrono>
#include <coroutine>
#include <system_error>

#include <liburing.h>

namespace fiona {

namespace detail {

BOOST_NOINLINE BOOST_NORETURN inline void
throw_busy() {
  detail::throw_errno_as_error_code( EBUSY );
}

} // namespace detail

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

    timeout_frame( executor ex, timer_impl* ptimer )
        : ex_{ ex }, ptimer_{ ptimer } {}

    ~timeout_frame() {}

    void reset() {
      ts_ = { .tv_sec = 0, .tv_nsec = 0 };
      h_ = nullptr;
      initiated_ = false;
      done_ = false;
      ec_ = {};
    }

    void await_process_cqe( io_uring_cqe* cqe ) override {
      done_ = true;
      auto e = -cqe->res;
      if ( e != 0 && e != ETIME ) {
        ec_ = error_code{ std::make_error_code( static_cast<std::errc>( e ) ) };
      }
    }

    std::coroutine_handle<> handle() noexcept override {
      BOOST_ASSERT( h_ );
      auto h = h_;
      h_ = nullptr;
      return h;
    }

    void inc_ref() noexcept override { ++ptimer_->count_; }
    void dec_ref() noexcept override {
      --ptimer_->count_;
      if ( ptimer_->count_ == 0 ) {
        delete ptimer_;
      }
    }

    int use_count() const noexcept override { return ptimer_->count_; }
  };

  struct cancel_frame : public detail::awaitable_base {
    error_code ec_;
    executor ex_;
    timer_impl* ptimer_ = nullptr;
    std::coroutine_handle<> h_ = nullptr;
    bool initiated_ = false;
    bool done_ = false;

    cancel_frame( executor ex, timer_impl* ptimer )
        : ex_{ ex }, ptimer_{ ptimer } {}

    ~cancel_frame() {}

    void await_process_cqe( io_uring_cqe* cqe ) override {
      done_ = true;
      if ( cqe->res < 0 ) {
        ec_ = error_code::from_errno( -cqe->res );
      }
    }

    std::coroutine_handle<> handle() noexcept override {
      auto h = h_;
      h_ = nullptr;
      return h;
    }

    void inc_ref() noexcept override { ++ptimer_->count_; }
    void dec_ref() noexcept override {
      --ptimer_->count_;
      if ( ptimer_->count_ == 0 ) {
        delete ptimer_;
      }
    }
    int use_count() const noexcept override { return ptimer_->count_; }
  };

  timeout_frame tf_;
  cancel_frame cf_;
  int count_ = 0;

  friend inline void intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept;
  friend inline void intrusive_ptr_release( timer_impl* ptimer ) noexcept;

public:
  timer_impl( executor ex ) : tf_{ ex, this }, cf_{ ex, this } {}
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
                   __kernel_timespec ts )
      : ptimer_{ ptimer } {
    ptimer_->tf_.ts_ = ts;
  }

public:
  ~timer_awaitable() {
    auto& frame = ptimer_->tf_;
    if ( frame.initiated_ && !frame.done_ ) {
      auto ring = detail::executor_access_policy::ring( frame.ex_ );

      auto sqe = detail::get_sqe( ring );
      io_uring_prep_cancel( sqe, &frame, 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_submit( ring );
    }
  }

  bool await_ready() const {
    if ( ptimer_->tf_.initiated_ ) {
      detail::throw_busy();
    }
    return false;
  }

  void await_suspend( std::coroutine_handle<> h ) {
    auto& frame = ptimer_->tf_;
    if ( frame.initiated_ ) {
      detail::throw_busy();
    }

    frame.h_ = h;

    auto ring = detail::executor_access_policy::ring( frame.ex_ );
    auto sqe = detail::get_sqe( ring );

    io_uring_prep_timeout( sqe, &frame.ts_, 0, 0 );
    io_uring_sqe_set_data( sqe, boost::intrusive_ptr( &frame ).detach() );

    frame.initiated_ = true;
  }

  result<void> await_resume() {
    auto& f = ptimer_->tf_;
    auto ec = std::move( f.ec_ );
    f.reset();
    if ( ec ) {
      return { ec };
    }
    return {};
  }
};

struct timer_cancel_awaitable {
private:
  friend struct timer;

  boost::intrusive_ptr<timer_impl> ptimer_ = nullptr;

  timer_cancel_awaitable( boost::intrusive_ptr<timer_impl> ptimer )
      : ptimer_{ ptimer } {}

public:
  ~timer_cancel_awaitable() {
    auto& frame = ptimer_->cf_;
    if ( frame.initiated_ && !frame.done_ ) {
      auto ring = detail::executor_access_policy::ring( frame.ex_ );

      auto sqe = detail::get_sqe( ring );
      io_uring_prep_cancel( sqe, &frame, 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_submit( ring );
    }
  }

  bool await_ready() const { return false; }

  void await_suspend( std::coroutine_handle<> h ) {
    auto& frame = ptimer_->cf_;

    auto ring = detail::executor_access_policy::ring( frame.ex_ );
    auto sqe = detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, &ptimer_->tf_, 0 );
    io_uring_sqe_set_data( sqe, boost::intrusive_ptr( &frame ).detach() );

    frame.h_ = h;
    frame.initiated_ = true;
  }

  result<void> await_resume() {
    auto& f = ptimer_->cf_;
    auto ec = std::move( f.ec_ );
    if ( ec ) {
      return { ec };
    }
    return {};
  }
};

struct timer {
private:
  boost::intrusive_ptr<timer_impl> ptimer_ = nullptr;

public:
  timer( executor ex ) : ptimer_{ new timer_impl( ex ) } {}

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

  timer_cancel_awaitable async_cancel() { return { ptimer_ }; }

  executor get_executor() const noexcept { return ptimer_->tf_.ex_; }
};

} // namespace fiona

#endif // FIONA_TIME_HPP
