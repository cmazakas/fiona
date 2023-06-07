#include <boost/core/lightweight_test.hpp>

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
  std::suspend_never final_suspend() noexcept { return {}; }
  void return_void() {}
  void unhandled_exception() {}
};

task test_coro() { co_return; }

int main() {
  io_uring ioring;
  auto *ring = &ioring;

  {
    unsigned const entries = 32;
    unsigned const flags = 0;
    io_uring_queue_init(entries, ring, flags);
  }

  auto t = test_coro();

  io_uring_queue_exit(ring);
}
