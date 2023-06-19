#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

#include <boost/assert.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <coroutine>
#include <cstddef>

#include <liburing.h>

namespace fiona {

struct task;
struct promise;

namespace detail {
struct io_context_frame;
}

struct executor {
private:
  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  executor(boost::local_shared_ptr<detail::io_context_frame> framep) noexcept
      : framep_(std::move(framep)) {}

  io_uring* ring() const noexcept;
  void post(task t);
};

struct task {
private:
  struct awaitable {
    std::coroutine_handle<promise> h_;

    bool await_ready() const noexcept { return !h_ || h_.done(); }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> awaiting_coro) noexcept;

    void await_resume() noexcept {}
  };

  std::coroutine_handle<promise> h_ = nullptr;

public:
  using promise_type = ::fiona::promise;

  task() = default;
  task(std::coroutine_handle<promise> h) : h_(h) {}

  ~task() {
    if (h_) {
      h_.destroy();
    }
  }

  task(task const&) = delete;
  task& operator=(task const&) = delete;

  task(task&& rhs) noexcept : h_(rhs.h_) { rhs.h_ = nullptr; }
  task& operator=(task&& rhs) noexcept {
    if (this != &rhs) {
      auto h = h_;
      h_ = rhs.h_;
      rhs.h_ = h;
    }
    return *this;
  }

  awaitable operator co_await() noexcept { return awaitable{h_}; }
};

struct promise {
private:
  struct final_awaitable {
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<promise> coro) noexcept {
      return coro.promise().continuation_;
    }

    void await_resume() noexcept { BOOST_ASSERT(false); }
  };

  std::coroutine_handle<> continuation_ = nullptr;

public:
  task get_return_object() {
    return {std::coroutine_handle<promise>::from_promise(*this)};
  }

  std::suspend_always initial_suspend() { return {}; }
  final_awaitable final_suspend() noexcept { return {}; }

  void return_void() {}
  void unhandled_exception() {}

  void set_continuation(std::coroutine_handle<> continuation) {
    BOOST_ASSERT(!continuation_);
    continuation_ = continuation;
  }
};

std::coroutine_handle<>
task::awaitable::await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
  /*
   * because this awaitable is created using the coroutine_handle of a child
   * coroutine, awaiting_coro is the parent
   *
   * store a handle to the parent in the Promise object so that we can
   * access it later in Promise::final_suspend.
   */
  h_.promise().set_continuation(awaiting_coro);
  return h_;
}

namespace detail {

struct internal_promise;

struct internal_task {
  using promise_type = internal_promise;

  std::coroutine_handle<promise_type> h_;

  internal_task(std::coroutine_handle<promise_type> h) : h_(h) {}
  ~internal_task() {
    if (h_) {
      h_.destroy();
    }
  }

  internal_task(internal_task const&) = delete;
  internal_task& operator=(internal_task const&) = delete;

  internal_task(internal_task&& rhs) noexcept : h_(rhs.h_) { rhs.h_ = nullptr; }
  internal_task& operator=(internal_task&& rhs) noexcept {
    if (this != &rhs) {
      auto h = h_;
      h_ = rhs.h_;
      rhs.h_ = h;
    }
    return *this;
  }

  friend std::size_t hash_value(internal_task const& t) noexcept {
    boost::hash<void*> hasher;
    return hasher(t.h_.address());
  }

  friend bool operator==(internal_task const& lhs, internal_task const& rhs) {
    return lhs.h_.address() == rhs.h_.address();
  }
};

struct internal_promise {
  internal_task get_return_object() {
    return internal_task(
        std::coroutine_handle<internal_promise>::from_promise(*this));
  }

  std::suspend_always initial_suspend() { return {}; }
  std::suspend_always final_suspend() noexcept { return {}; }

  void unhandled_exception() {}
  void return_void() {}
};

struct hasher {
  using is_transparent = void;

  template <class T> std::size_t operator()(T const& t) const noexcept {
    boost::hash<T> hasher;
    return hasher(t);
  }
};

struct key_equal {
  using is_transparent = void;

  template <class T, class U> bool operator()(T const& t, U const& u) const {
    return t == u;
  }

  bool operator()(internal_task const& t, void* p) const noexcept {
    return t.h_.address() == p;
  }

  bool operator()(void* p, internal_task const& t) const noexcept {
    return t.h_.address() == p;
  }
};

struct io_context_frame {
  friend struct executor;

  boost::unordered_flat_set<internal_task, hasher, key_equal> tasks_;
  io_uring io_ring_;

  io_context_frame() {
    unsigned const entries = 32;
    unsigned const flags = 0;
    io_uring_queue_init(entries, &io_ring_, flags);
  }

  ~io_context_frame() { io_uring_queue_exit(&io_ring_); }
};

} // namespace detail

io_uring*
executor::ring() const noexcept {
  return &framep_->io_ring_;
}

detail::internal_task
scheduler(io_uring* ring, task t) {
  struct this_coro_awaitable {
    std::coroutine_handle<> h_ = nullptr;

    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
      h_ = h;
      return false;
    }

    auto await_resume() { return h_; }
  };

  co_await t;

  auto h = co_await this_coro_awaitable{};
  auto sqe = io_uring_get_sqe(ring);
  io_uring_prep_nop(sqe);

  sqe->user_data = reinterpret_cast<std::uintptr_t>(h.address());

  io_uring_submit(ring);
  co_return;
}

void
executor::post(task t) {
  auto it = scheduler(ring(), std::move(t));

  auto sqe = io_uring_get_sqe(ring());
  io_uring_prep_nop(sqe);
  sqe->user_data = reinterpret_cast<std::uintptr_t>(it.h_.address());
  io_uring_submit(ring());

  framep_->tasks_.insert(std::move(it));
}

struct io_context {
private:
  struct cqe_guard {
    io_uring* ring;
    io_uring_cqe* cqe;

    ~cqe_guard() { io_uring_cqe_seen(ring, cqe); }
  };

  decltype(auto) tasks() const noexcept { return (framep_->tasks_); }

  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  io_context()
      : framep_(boost::make_local_shared<detail::io_context_frame>()) {}

  executor get_executor() const noexcept { return executor{framep_}; }

  void post(task t) {
    auto ex = get_executor();
    ex.post(std::move(t));
  }

  void run() {
    auto ex = get_executor();

    while (!tasks().empty()) {
      auto p = ([&ex] {
        io_uring_cqe* cqe = nullptr;
        io_uring_wait_cqe(ex.ring(), &cqe);
        auto guard = cqe_guard(ex.ring(), cqe);

        auto p = reinterpret_cast<void*>(cqe->user_data);
        return p;
      })();

      auto h = std::coroutine_handle<>::from_address(p);

      if (auto pos = tasks().find(p); pos != tasks().end()) {
        if (h.done()) {
          tasks().erase(pos);
          continue;
        }
      }

      h.resume();
    }
  }
};

} // namespace fiona

#endif // FIONA_IO_CONTEXT_HPP
