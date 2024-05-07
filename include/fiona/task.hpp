#ifndef FIONA_TASK_HPP
#define FIONA_TASK_HPP

#include <boost/config.hpp>
#include <boost/core/exchange.hpp>
#include <coroutine>        // for coroutine_handle, suspend_always
#include <exception>        // for exception_ptr, rethrow_exception, curren...
#include <utility>          // for addressof, move, forward

#include <boost/assert.hpp> // for BOOST_ASSERT

#if BOOST_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-default"
#endif

namespace fiona {
template <class T>
struct promise;
} // namespace fiona

namespace fiona {

template <class T>
struct task {
private:
  struct awaitable {
    std::coroutine_handle<promise<T>> h_;

    awaitable( std::coroutine_handle<promise<T>> h ) : h_( h ) {}

    bool await_ready() const noexcept { return !h_ || h_.done(); }

    std::coroutine_handle<>
    await_suspend( std::coroutine_handle<> awaiting_coro ) noexcept;

    decltype( auto ) await_resume() {
      BOOST_ASSERT( h_ );
      return std::move( h_.promise() ).result();
    }
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

  void* into_address() {
    BOOST_ASSERT( h_ );
    auto p = h_.address();
    h_ = nullptr;
    return p;
  }

  static task<void> from_address( void* p ) {
    return { std::coroutine_handle<promise<void>>::from_address( p ) };
  }
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

    BOOST_NORETURN
    void await_resume() noexcept {
      BOOST_ASSERT( false );
      __builtin_unreachable();
    }
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

    case result_type::uninit:
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

#if BOOST_CLANG
#pragma clang diagnostic pop
#endif

#endif // FIONA_TASK_HPP
