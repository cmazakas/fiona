#include <boost/core/lightweight_test.hpp>

#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>
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

namespace fiona {

namespace detail {

struct io_context_frame {
  boost::unordered_flat_map<std::uint64_t, task> tasks_;
  io_uring io_ring_;

  io_context_frame() {
    unsigned const entries = 32;
    unsigned const flags = 0;
    io_uring_queue_init(entries, &io_ring_, flags);
  }

  ~io_context_frame() { io_uring_queue_exit(&io_ring_); }
};

} // namespace detail

struct executor {
private:
  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  executor(boost::local_shared_ptr<detail::io_context_frame> framep) noexcept
      : framep_(std::move(framep)) {}

  io_uring* ring() const noexcept { return &framep_->io_ring_; }

  void post(task t) {
    framep_->tasks_.insert({reinterpret_cast<std::uintptr_t>(t.address()), t});

    auto sqe = io_uring_get_sqe(ring());
    io_uring_prep_nop(sqe);
    sqe->user_data = reinterpret_cast<std::uintptr_t>(t.address());
    io_uring_submit(ring());
  }
};

struct io_context {
private:
  boost::local_shared_ptr<detail::io_context_frame> framep_;

  struct cqe_guard {
    io_uring* ring;
    io_uring_cqe* cqe;

    ~cqe_guard() { io_uring_cqe_seen(ring, cqe); }
  };

  boost::unordered_flat_map<std::uint64_t, task>& tasks() const noexcept {
    return framep_->tasks_;
  }

public:
  io_context()
      : framep_(boost::make_local_shared<detail::io_context_frame>()) {}

  executor get_executor() const noexcept { return executor{framep_}; }

  void post(task t) {
    auto ex = get_executor();
    ex.post(t);
  }

  void run() {
    auto ex = get_executor();

    while (!tasks().empty()) {
      io_uring_cqe* cqe = nullptr;
      io_uring_wait_cqe(ex.ring(), &cqe);
      auto guard = cqe_guard(ex.ring(), cqe);

      auto pos = tasks().find(cqe->user_data);
      if (pos == tasks().end()) {
        tasks().erase(pos);
        continue;
      }

      auto handle = std::coroutine_handle<>::from_address(
          reinterpret_cast<void*>(cqe->user_data));

      handle.resume();
      if (handle.done()) {
        handle.destroy();
        tasks().erase(cqe->user_data);
      }
    }
  }
};

} // namespace fiona

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
    io_uring_prep_timeout(sqe, &ts, 1, 0);
    sqe->user_data = reinterpret_cast<std::uintptr_t>(h.address());
    io_uring_submit(ring);
  }

  void await_resume() {}
};

timer_awaitable
sleep_for(fiona::executor ex, std::chrono::seconds sec,
          std::chrono::nanoseconds nsec) {
  return {ex.ring(), sec.count(), nsec.count()};
}

task
test_coro(fiona::executor ex) {
  co_await sleep_for(ex, std::chrono::seconds(2),
                     std::chrono::milliseconds(500));
  co_return;
}

int
main() {
  fiona::io_context ioc;
  ioc.post(test_coro(ioc.get_executor()));
  ioc.run();

  return boost::report_errors();
}
