#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <coroutine>
#include <iostream>

#include <liburing.h>

struct promise;

struct task : std::coroutine_handle<promise> {
  using promise_type = ::promise;
};

struct promise {
  task get_return_object() {
    return {std::coroutine_handle<promise>::from_promise(*this)};
  }

  std::suspend_always initial_suspend() { return {}; }
  std::suspend_always final_suspend() noexcept { return {}; }
  void return_void() {}
  void unhandled_exception() {}
};

struct timer_awaitable {
  io_uring *ring = nullptr;
  long long sec = 0;
  long long nsec = 0;

  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    auto sqe = io_uring_get_sqe(ring);

    __kernel_timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    io_uring_prep_timeout(sqe, &ts, 1, 0);
    io_uring_submit(ring);
  }

  void await_resume() {}
};

timer_awaitable
sleep_for(io_uring *ring, std::chrono::seconds sec,
          std::chrono::nanoseconds nsec) {
  return {ring, sec.count(), nsec.count()};
}

task
test_coro(io_uring *ring) {
  co_await sleep_for(ring, std::chrono::seconds(1),
                     std::chrono::nanoseconds(0));
  co_return;
}

int
main() {
  io_uring ioring;
  auto *ring = &ioring;

  {
    unsigned const entries = 32;
    unsigned const flags = 0;
    io_uring_queue_init(entries, ring, flags);
  }

  auto t = test_coro(ring);
  BOOST_TEST(!t.done());
  t.resume();
  BOOST_TEST(!t.done());

  io_uring_cqe *cqe = nullptr;
  io_uring_wait_cqe(ring, &cqe);
  io_uring_cqe_seen(ring, cqe);

  t.resume();
  BOOST_TEST(t.done());

  if (t.done()) {
    t.destroy();
  }

  io_uring_queue_exit(ring);

  return boost::report_errors();
}
