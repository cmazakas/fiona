#ifndef FIONA_SLEEP_HPP
#define FIONA_SLEEP_HPP

#include <fiona/detail/awaitable_base.hpp>
#include <fiona/error.hpp>
#include <fiona/io_context.hpp>

#include <chrono>
#include <coroutine>
#include <system_error>

#include <liburing.h>

namespace fiona {

struct timer_awaitable final : public detail::awaitable_base {
  io_uring* ring = nullptr;
  long long sec = 0;
  long long nsec = 0;
  std::coroutine_handle<> h = nullptr;
  std::error_code ec;

  timer_awaitable( io_uring* ring_, long long sec_, long long nsec_ )
      : ring( ring_ ), sec{ sec_ }, nsec{ nsec_ } {}

  bool await_ready() { return false; }
  void await_suspend( std::coroutine_handle<> h_ ) {
    h = h_;

    auto sqe = io_uring_get_sqe( ring );

    __kernel_timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    io_uring_prep_timeout( sqe, &ts, 0, 0 );
    sqe->user_data = reinterpret_cast<std::uintptr_t>( this );
    io_uring_submit( ring );
  }

  error_code await_resume() { return { std::move( ec ) }; }

  void await_process_cqe( io_uring_cqe* cqe ) {
    auto e = -cqe->res;
    if ( e != 0 && e != ETIME ) {
      ec = std::make_error_code( static_cast<std::errc>( e ) );
    }
  }

  std::coroutine_handle<> handle() const noexcept { return h; }
};

template <class Rep, class Period>
timer_awaitable
sleep_for( fiona::executor ex, std::chrono::duration<Rep, Period> d ) {
  auto sec = std::chrono::floor<std::chrono::seconds>( d );
  auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>( d - sec );
  return timer_awaitable( ex.ring(), sec.count(), nsec.count() );
}

} // namespace fiona

#endif // FIONA_SLEEP_HPP
