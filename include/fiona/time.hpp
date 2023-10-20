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

  struct frame final : public detail::awaitable_base {
    error_code ec_;
    __kernel_timespec ts_ = { .tv_sec = 0, .tv_nsec = 0 };
    executor ex_;
    timer_impl* ptimer_ = nullptr;
    std::coroutine_handle<> h_;
    bool initiated_ = false;
    bool done_ = false;

    frame( executor ex, timer_impl* ptimer ) : ex_{ ex }, ptimer_{ ptimer } {}
    ~frame() {}

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

  frame f_;
  int count_ = 0;

  friend inline void intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept;
  friend inline void intrusive_ptr_release( timer_impl* ptimer ) noexcept;

public:
  timer_impl( executor ex ) : f_{ ex, this } {}
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
    ptimer_->f_.ts_ = ts;
  }

public:
  ~timer_awaitable() {
    auto& frame = ptimer_->f_;
    if ( frame.initiated_ && !frame.done_ ) {
      auto ring = detail::executor_access_policy::ring( frame.ex_ );

      auto sqe = detail::get_sqe( ring );
      io_uring_prep_cancel( sqe, &frame, 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_submit( ring );
    }
  }

  bool await_ready() const {
    if ( ptimer_->f_.initiated_ ) {
      detail::throw_busy();
    }
    return false;
  }

  void await_suspend( std::coroutine_handle<> h ) {
    auto& frame = ptimer_->f_;
    if ( frame.initiated_ ) {
      detail::throw_busy();
    }

    frame.h_ = h;

    auto ring = detail::executor_access_policy::ring( frame.ex_ );
    auto sqe = detail::get_sqe( ring );

    io_uring_prep_timeout( sqe, &frame.ts_, 0, 0 );
    io_uring_sqe_set_data( sqe, boost::intrusive_ptr( ptimer_ ).detach() );

    frame.initiated_ = true;
  }

  result<void> await_resume() {
    auto& f = ptimer_->f_;
    auto ec = std::move( f.ec_ );
    f.reset();
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
};

} // namespace fiona

#endif // FIONA_TIME_HPP
