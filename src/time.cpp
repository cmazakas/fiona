#include <fiona/time.hpp>

#include <fiona/error.hpp>                   // for error_code, throw_errno_as_error_code, result
#include <fiona/executor.hpp>                // for executor, executor_access_policy

#include <fiona/detail/awaitable_base.hpp>   // for awaitable_base
#include <fiona/detail/config.hpp>           // for FIONA_DECL
#include <fiona/detail/get_sqe.hpp>          // for get_sqe, submit_ring

#include <boost/assert.hpp>                  // for BOOST_ASSERT
#include <boost/config/detail/suffix.hpp>    // for BOOST_NOINLINE, BOOST_NORETURN
#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <coroutine>                         // for coroutine_handle
#include <system_error>                      // for make_error_code, errc
#include <utility>                           // for move

#include <errno.h>                           // for EBUSY, ETIME
#include <liburing.h>                        // for io_uring_sqe_set_data, io_uring_prep_cancel, io_uring_prep_timeout
#include <liburing/io_uring.h>               // for io_uring_cqe
#include <linux/time_types.h>                // for __kernel_timespec

namespace fiona {

namespace {
BOOST_NOINLINE BOOST_NORETURN inline void
throw_busy() {
  detail::throw_errno_as_error_code( EBUSY );
}
} // namespace

namespace detail {
struct timer_impl {
private:
  friend struct fiona::timer_awaitable;
  friend struct fiona::timer_cancel_awaitable;
  friend struct fiona::timer;

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

  friend void intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept;
  friend void intrusive_ptr_release( timer_impl* ptimer ) noexcept;

public:
  timer_impl( executor ex );
};

void FIONA_DECL
intrusive_ptr_add_ref( timer_impl* ptimer ) noexcept {
  ++ptimer->count_;
}

void FIONA_DECL
intrusive_ptr_release( timer_impl* ptimer ) noexcept {
  --ptimer->count_;
  if ( ptimer->count_ == 0 ) {
    delete ptimer;
  }
}

timer_impl::timeout_frame::timeout_frame( executor ex, timer_impl* ptimer ) : ex_{ ex }, ptimer_{ ptimer } {}

timer_impl::timeout_frame::~timeout_frame() {}

void
timer_impl::timeout_frame::reset() {
  ts_ = { .tv_sec = 0, .tv_nsec = 0 };
  h_ = nullptr;
  initiated_ = false;
  done_ = false;
  ec_ = {};
}

void
timer_impl::timeout_frame::await_process_cqe( io_uring_cqe* cqe ) {
  done_ = true;
  auto e = -cqe->res;
  if ( e != 0 && e != ETIME ) {
    ec_ = error_code{ std::make_error_code( static_cast<std::errc>( e ) ) };
  }
}

std::coroutine_handle<>
timer_impl::timeout_frame::handle() noexcept {
  BOOST_ASSERT( h_ );
  auto h = h_;
  h_ = nullptr;
  return h;
}

void
timer_impl::timeout_frame::inc_ref() noexcept {
  ++ptimer_->count_;
}

void
timer_impl::timeout_frame::dec_ref() noexcept {
  --ptimer_->count_;
  if ( ptimer_->count_ == 0 ) {
    delete ptimer_;
  }
}

int
timer_impl::timeout_frame::use_count() const noexcept {
  return ptimer_->count_;
}

timer_impl::cancel_frame::cancel_frame( executor ex, timer_impl* ptimer ) : ex_{ ex }, ptimer_{ ptimer } {}

timer_impl::cancel_frame::~cancel_frame() {}

void
timer_impl::cancel_frame::reset() {
  initiated_ = false;
  done_ = false;
  ec_ = {};
}

void
timer_impl::cancel_frame::await_process_cqe( io_uring_cqe* cqe ) {
  done_ = true;
  if ( cqe->res < 0 ) {
    ec_ = error_code::from_errno( -cqe->res );
  }
}

std::coroutine_handle<>
timer_impl::cancel_frame::handle() noexcept {
  auto h = h_;
  h_ = nullptr;
  return h;
}

void
timer_impl::cancel_frame::inc_ref() noexcept {
  ++ptimer_->count_;
}

void
timer_impl::cancel_frame::dec_ref() noexcept {
  --ptimer_->count_;
  if ( ptimer_->count_ == 0 ) {
    delete ptimer_;
  }
}

int
timer_impl::cancel_frame::use_count() const noexcept {
  return ptimer_->count_;
}

timer_impl::timer_impl( executor ex ) : tf_{ ex, this }, cf_{ ex, this } {}
} // namespace detail

timer_awaitable::timer_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer, __kernel_timespec ts )
    : ptimer_{ ptimer } {
  ptimer_->tf_.ts_ = ts;
}

timer_awaitable::~timer_awaitable() {
  auto& frame = ptimer_->tf_;
  if ( frame.initiated_ && !frame.done_ ) {
    auto ring = detail::executor_access_policy::ring( frame.ex_ );

    auto sqe = detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, &frame, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    fiona::detail::submit_ring( ring );
  }
}

bool
timer_awaitable::await_ready() const {
  if ( ptimer_->tf_.initiated_ ) {
    throw_busy();
  }
  return false;
}

void
timer_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& frame = ptimer_->tf_;
  if ( frame.initiated_ ) {
    throw_busy();
  }

  frame.h_ = h;

  auto ring = detail::executor_access_policy::ring( frame.ex_ );
  auto sqe = detail::get_sqe( ring );

  io_uring_prep_timeout( sqe, &frame.ts_, 0, 0 );
  io_uring_sqe_set_data( sqe, boost::intrusive_ptr( &frame ).detach() );

  frame.initiated_ = true;
}

result<void>
timer_awaitable::await_resume() {
  auto& f = ptimer_->tf_;
  auto ec = std::move( f.ec_ );
  f.reset();
  if ( ec ) {
    return { ec };
  }
  return {};
}

timer_cancel_awaitable::timer_cancel_awaitable( boost::intrusive_ptr<detail::timer_impl> ptimer ) : ptimer_{ ptimer } {}

timer_cancel_awaitable::~timer_cancel_awaitable() {
  auto& frame = this->ptimer_->cf_;
  if ( frame.initiated_ && frame.done_ ) {
    auto ring = detail::executor_access_policy::ring( frame.ex_ );
    auto sqe = detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, &frame, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    fiona::detail::submit_ring( ring );
  }
}

bool
timer_cancel_awaitable::await_ready() const {
  if ( ptimer_->cf_.initiated_ ) {
    throw_busy();
  }
  return false;
}

void
timer_cancel_awaitable::await_suspend( std::coroutine_handle<> h ) {
  if ( ptimer_->cf_.initiated_ ) {
    throw_busy();
  }

  auto& frame = ptimer_->cf_;

  auto ring = detail::executor_access_policy::ring( frame.ex_ );
  auto sqe = detail::get_sqe( ring );
  io_uring_prep_cancel( sqe, &ptimer_->tf_, 0 );
  io_uring_sqe_set_data( sqe, boost::intrusive_ptr( &frame ).detach() );

  frame.h_ = h;
  frame.initiated_ = true;
}

result<void>
timer_cancel_awaitable::await_resume() {
  auto& f = ptimer_->cf_;
  if ( !f.initiated_ || !f.done_ ) {
    throw_busy();
  }

  auto ec = std::move( f.ec_ );
  f.reset();
  if ( ec ) {
    return { ec };
  }
  return {};
}

timer::timer( executor ex ) : ptimer_{ new detail::timer_impl( ex ) } {}

timer_cancel_awaitable
timer::async_cancel() {
  return { ptimer_ };
}

executor
timer::get_executor() const noexcept {
  return ptimer_->tf_.ex_;
}

} // namespace fiona
