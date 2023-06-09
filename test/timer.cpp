#include <boost/core/lightweight_test.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
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
    sqe->user_data = reinterpret_cast<std::uintptr_t>(h.address());
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
  co_await sleep_for(ring, std::chrono::seconds(2),
                     std::chrono::milliseconds(500));
  co_return;
}

namespace fiona {

struct io_context {
private:
  boost::unordered_flat_map<std::uint64_t, task> tasks_;
  io_uring io_ring_;
  io_uring *ring_ = nullptr;

  struct cqe_guard {
    io_uring *ring;
    io_uring_cqe *cqe;

    ~cqe_guard() { io_uring_cqe_seen(ring, cqe); }
  };

public:
  io_context() {
    unsigned const entries = 32;
    unsigned const flags = 0;
    io_uring_queue_init(entries, &io_ring_, flags);
    ring_ = &io_ring_;
  }

  ~io_context() { io_uring_queue_exit(ring_); }

  void post(task t) {
    tasks_.insert({reinterpret_cast<std::uintptr_t>(t.address()), t});

    auto sqe = io_uring_get_sqe(ring_);
    io_uring_prep_nop(sqe);
    sqe->user_data = reinterpret_cast<std::uintptr_t>(t.address());
    io_uring_submit(ring_);
  }

  io_uring *ring() { return ring_; }

  void run() {
    while (!tasks_.empty()) {
      io_uring_cqe *cqe = nullptr;
      io_uring_wait_cqe(ring_, &cqe);
      auto guard = cqe_guard(ring_, cqe);

      auto pos = tasks_.find(cqe->user_data);
      if (pos == tasks_.end()) {
        tasks_.erase(pos);
        continue;
      }

      auto handle = std::coroutine_handle<>::from_address(
          reinterpret_cast<void *>(cqe->user_data));

      handle.resume();
      if (handle.done()) {
        handle.destroy();
        tasks_.erase(cqe->user_data);
      }
    }
  }
};

} // namespace fiona

int
main() {
  fiona::io_context ioc;
  ioc.post(test_coro(ioc.ring()));
  ioc.run();

  return boost::report_errors();
}
