#ifndef FIONA_SLEEP_HPP
#define FIONA_SLEEP_HPP

#include <chrono>

#include <liburing.h>

namespace fiona {
struct timer_awaitable {
  io_uring* ring = nullptr;
  long long sec = 0;
  long long nsec = 0;

  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    auto sqe = io_uring_get_sqe(ring);

    __kernel_timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    io_uring_prep_timeout(sqe, &ts, UINT_MAX, 0);
    sqe->user_data = reinterpret_cast<std::uintptr_t>(h.address());
    io_uring_submit(ring);
  }

  void await_resume() {}
};

template <class Rep, class Period>
timer_awaitable
sleep_for(fiona::executor ex, std::chrono::duration<Rep, Period> d) {
  auto sec = std::chrono::floor<std::chrono::seconds>(d);
  auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(d - sec);
  return timer_awaitable{ex.ring(), sec.count(), nsec.count()};
}
} // namespace fiona

#endif // FIONA_SLEEP_HPP
