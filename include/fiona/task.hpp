#ifndef FIONA_TASK_HPP
#define FIONA_TASK_HPP

#include <fiona/detail/awaitable_base.hpp>

#include <coroutine>

namespace fiona {

template <class T>
struct promise;

template <class T>
struct task {
private:
  struct awaitable final : public detail::awaitable_base {
    std::coroutine_handle<promise<T>> h_;

    awaitable( std::coroutine_handle<promise<T>> h ) : h_( h ) {}

    bool await_ready() const noexcept { return !h_ || h_.done(); }

    std::coroutine_handle<>
    await_suspend( std::coroutine_handle<> awaiting_coro ) noexcept;

    decltype( auto ) await_resume() {
      BOOST_ASSERT( h_ );
      return h_.promise().result();
    }

    void await_process_cqe( io_uring_cqe* ) {}
    std::coroutine_handle<> handle() noexcept { return h_; }
  };

  std::coroutine_handle<promise<T>> h_ = nullptr;

public:
  using promise_type = ::fiona::promise<T>;

  task() = default;
  task( std::coroutine_handle<promise<T>> h ) : h_( h ) {}

  ~task() {
    if ( h_ ) {
      h_.destroy();
    }
  }

  task( task const& ) = delete;
  task& operator=( task const& ) = delete;

  task( task&& rhs ) noexcept : h_( rhs.h_ ) { rhs.h_ = nullptr; }
  task& operator=( task&& rhs ) noexcept {
    if ( this != &rhs ) {
      auto h = h_;
      h_ = rhs.h_;
      rhs.h_ = h;
    }
    return *this;
  }

  awaitable operator co_await() noexcept { return awaitable{ h_ }; }
};

struct promise_base {
private:
  struct final_awaitable {
    bool await_ready() const noexcept { return false; }

    template <class Promise>
    std::coroutine_handle<>
    await_suspend( std::coroutine_handle<Promise> coro ) noexcept {
      return coro.promise().continuation_;
    }

    void await_resume() noexcept { BOOST_ASSERT( false ); }
  };

  std::coroutine_handle<> continuation_ = nullptr;

public:
  promise_base() = default;

  std::suspend_always initial_suspend() { return {}; }
  final_awaitable final_suspend() noexcept { return {}; }

  void set_continuation( std::coroutine_handle<> continuation ) {
    BOOST_ASSERT( !continuation_ );
    continuation_ = continuation;
  }
};

template <class T>
struct promise final : public promise_base {
private:
  enum class result_type { uninit, ok, err };

  result_type rt = result_type::uninit;
  union {
    T value_;
    std::exception_ptr exception_;
  };

public:
  promise() noexcept {}
  ~promise() {
    switch ( rt ) {
    case result_type::ok:
      value_.~T();
      break;

    case result_type::err:
      exception_.~exception_ptr();
      break;

    default:
      BOOST_ASSERT( false );
      break;
    }
  }

  task<T> get_return_object() {
    return { std::coroutine_handle<promise>::from_promise( *this ) };
  }

  template <class Expr>
  void return_value( Expr&& expr ) {
    new ( std::addressof( value_ ) ) T( std::forward<Expr>( expr ) );
    rt = result_type::ok;
  }

  void unhandled_exception() {
    new ( std::addressof( exception_ ) )
        std::exception_ptr( std::current_exception() );
    rt = result_type::err;
  }

  T& result() & {
    if ( rt == result_type::err ) {
      std::rethrow_exception( exception_ );
    }
    return value_;
  }

  T&& result() && {
    if ( rt == result_type::err ) {
      std::rethrow_exception( exception_ );
    }
    return std::move( value_ );
  }
};

template <>
struct promise<void> final : public promise_base {
private:
  enum class result_type { uninit, ok, err };

  std::exception_ptr exception_;

public:
  task<void> get_return_object() {
    return { std::coroutine_handle<promise>::from_promise( *this ) };
  }

  void return_void() {}

  void unhandled_exception() {
    exception_ = std::exception_ptr( std::current_exception() );
  }

  void result() {
    if ( exception_ ) {
      std::rethrow_exception( exception_ );
    }
  }
};

template <class T>
std::coroutine_handle<>
task<T>::awaitable::await_suspend(
    std::coroutine_handle<> awaiting_coro ) noexcept {
  /*
   * because this awaitable is created using the coroutine_handle of a
   * child coroutine, awaiting_coro is the parent
   *
   * store a handle to the parent in the Promise object so that we can
   * access it later in Promise::final_suspend.
   */
  h_.promise().set_continuation( awaiting_coro );
  return h_;
}
} // namespace fiona

#endif // FIONA_TASK_HPP
