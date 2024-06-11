#ifndef FIONA_EXECUTOR_HPP
#define FIONA_EXECUTOR_HPP

#include <fiona/error.hpp>         // for throw_errno_as_error_code
#include <fiona/params.hpp>        // for io_context_params
#include <fiona/task.hpp>          // for task

#include <fiona/detail/common.hpp> // for io_context_frame, task_map_type, buf_ring

#include <boost/assert.hpp>        // for BOOST_ASSERT
#include <boost/container_hash/hash.hpp>          // for hash
#include <boost/unordered/unordered_flat_set.hpp> // for unordered_flat_set

#include <coroutine> // for coroutine_handle, noop_coroutine, suspend_always
#include <cstdint>   // for uint32_t
#include <cstring>   // for size_t
#include <deque>
#include <exception> // for exception_ptr, rethrow_exception, current_exception
#include <memory>    // for shared_ptr, __shared_ptr_access, weak_ptr
#include <mutex>     // for lock_guard, mutex
#include <utility>   // for move, addressof, forward, pair

#include <errno.h>   // for EINVAL, errno
#include <unistd.h>  // for write

#include "fiona_export.h"

namespace fiona {
namespace detail {
struct executor_access_policy;
}
} // namespace fiona

namespace fiona {
namespace detail {
template <class T>
struct internal_promise;
}
} // namespace fiona

namespace fiona {
template <class T>
struct spawn_awaitable;
}

struct io_uring;

namespace fiona {
namespace detail {
inline constexpr std::uintptr_t const wake_mask = 0b01;
inline constexpr std::uintptr_t const post_mask = 0b10;
inline constexpr std::uintptr_t const ptr_mask = ~( wake_mask + post_mask );
} // namespace detail

struct waker
{
  std::weak_ptr<void> p_;
  std::mutex& m_; // guess this dangles if the runtime dies; fix this
  int fd_ = -1;
  std::coroutine_handle<> h_;

  void
  wake() const
  {
    auto p = p_.lock();
    if ( !p ) {
      detail::throw_errno_as_error_code( EINVAL );
    }

    auto data = reinterpret_cast<std::uintptr_t>( h_.address() );
    data |= detail::wake_mask;

    ssize_t ret = -1;
    ret = write( fd_, &data, sizeof( data ) );
    if ( ret == -1 ) {
      detail::throw_errno_as_error_code( errno );
    }
  }
};

struct executor
{
private:
  friend struct detail::executor_access_policy;
  std::shared_ptr<detail::io_context_frame> pframe_;

public:
  executor( std::shared_ptr<detail::io_context_frame> pframe ) noexcept
      : pframe_( std::move( pframe ) )
  {
  }

  template <class T>
  spawn_awaitable<T> spawn( task<T> t );

  inline void post( task<void> t ) const;
  inline waker make_waker( std::coroutine_handle<> h ) const;

  void
  register_buffer_sequence( std::size_t num_bufs,
                            std::size_t buf_size,
                            std::uint16_t buffer_group_id )
  {
    auto ring = &pframe_->io_ring_;
    pframe_->buf_rings_.emplace_back( ring, num_bufs, buf_size,
                                      buffer_group_id );
  }
};

namespace detail {

struct executor_access_policy
{
  static inline io_uring*
  ring( executor ex ) noexcept
  {
    return &ex.pframe_->io_ring_;
  }

  static inline task_map_type&
  tasks( executor ex ) noexcept
  {
    return ex.pframe_->tasks_;
  }

  static inline std::deque<std::coroutine_handle<>>&
  run_queue( executor ex ) noexcept
  {
    return ex.pframe_->run_queue_;
  }

  static inline std::lock_guard<std::mutex>
  lock_guard( executor ex )
  {
    return std::lock_guard<std::mutex>( ex.pframe_->m_ );
  }

  static void
  unhandled_exception( executor ex, std::exception_ptr ep )
  {
    if ( !ex.pframe_->exception_ptr_ ) {
      ex.pframe_->exception_ptr_ = ep;
    }
  }

  static inline int
  get_available_fd( executor ex )
  {
    auto& fds = ex.pframe_->fds_;
    if ( fds.empty() ) {
      return -1;
    }
    auto pos = fds.begin();
    auto fd = *pos;
    fds.erase( pos );

    BOOST_ASSERT( fd >= 0 );
    BOOST_ASSERT( static_cast<std::uint32_t>( fd ) <
                  ex.pframe_->params_.num_files );
    return fd;
  }

  static inline void
  release_fd( executor ex, int fd )
  {
    if ( fd < 0 ) {
      return;
    }

    BOOST_ASSERT( static_cast<std::uint32_t>( fd ) <
                  ex.pframe_->params_.num_files );

    auto& fds = ex.pframe_->fds_;
    auto itb = fds.insert( fd );
    (void)itb;
    BOOST_ASSERT( itb.second );
  }

  static inline buf_ring*
  get_buffer_group( executor ex, std::size_t bgid ) noexcept
  {
    for ( auto& bg : ex.pframe_->buf_rings_ ) {
      if ( bgid == bg.bgid() ) {
        return &bg;
      }
    }
    return nullptr;
  }

  static inline int
  get_pipefd( executor ex )
  {
    return ex.pframe_->pipefd_[0];
  }

  static inline waker
  get_waker( executor ex, std::coroutine_handle<> h )
  {
    return { ex.pframe_, ex.pframe_->m_, ex.pframe_->pipefd_[1], h };
  }
};

template <class T>
struct internal_task
{
  using promise_type = internal_promise<T>;

  std::coroutine_handle<promise_type> h_ = nullptr;

  internal_task() = default;
  internal_task( std::coroutine_handle<promise_type> h ) : h_( h )
  {
    BOOST_ASSERT( h_.promise().count_ == 0 );
    h_.promise().count_ = 2;
  }

  ~internal_task()
  {
    if ( h_ && --h_.promise().count_ == 0 ) {
      h_.destroy();
    }
  }

  internal_task( internal_task const& rhs )
  {
    h_ = rhs.h_;
    ++h_.promise().count_;
  }

  internal_task&
  operator=( internal_task const& rhs )
  {
    if ( this != &rhs ) {
      if ( --h_.promise().count_ == 0 ) {
        h_.destroy();
      }

      h_ = rhs.h_;
      ++h_.promise().count_;
    }
    return *this;
  }

  internal_task( internal_task&& rhs ) noexcept : h_( rhs.h_ )
  {
    rhs.h_ = nullptr;
  }

  internal_task&
  operator=( internal_task&& rhs ) noexcept
  {
    if ( this != &rhs ) {
      auto h = h_;
      h_ = rhs.h_;
      rhs.h_ = h;
    }
    return *this;
  }
};

template <class T>
struct promise_variant
{
  enum class result_type { uninitialized, ok, error };

  union storage {
    T value_;
    std::exception_ptr exception_;

    storage() {}
    ~storage() {}
  };

  storage s_;
  result_type rt_ = result_type::uninitialized;

  ~promise_variant()
  {
    if ( rt_ == result_type::ok ) {
      s_.value_.~T();
    }

    if ( rt_ == result_type::error ) {
      s_.exception_.~exception_ptr();
    }
  }

  template <class... Args>
  void
  emplace( Args&&... args )
  {
    BOOST_ASSERT( rt_ == result_type::uninitialized );
    new ( std::addressof( s_.value_ ) ) T( std::forward<Args>( args )... );
    rt_ = result_type::ok;
  }

  void
  set_error()
  {
    new ( std::addressof( s_.exception_ ) )
        std::exception_ptr( std::current_exception() );
    rt_ = result_type::error;
  }

  bool
  has_error() const noexcept
  {
    return rt_ == result_type::error;
  }

  std::exception_ptr
  get_error() const
  {
    BOOST_ASSERT( rt_ == result_type::error );
    return s_.exception_;
  }

  T&
  result() &
  {
    if ( rt_ == result_type::error ) {
      std::rethrow_exception( s_.exception_ );
    }
    return s_.value_;
  }

  T&&
  result() &&
  {
    if ( rt_ == result_type::error ) {
      std::rethrow_exception( s_.exception_ );
    }
    return std::move( s_.value_ );
  }
};

template <>
struct promise_variant<void>
{
  enum class result_type { uninitialized, ok, error };

  std::exception_ptr exception_;
  result_type rt_ = result_type::uninitialized;

  template <class... Args>
  void
  emplace( Args&&... )
  {
  }

  void
  set_error()
  {
    exception_ = std::exception_ptr( std::current_exception() );
    rt_ = result_type::error;
  }

  bool
  has_error() const noexcept
  {
    return rt_ == result_type::error;
  }

  std::exception_ptr
  get_error() const
  {
    BOOST_ASSERT( rt_ == result_type::error );
    return exception_;
  }

  void
  result()
  {
    if ( exception_ ) {
      std::rethrow_exception( exception_ );
    }
  }
};

template <class T>
struct internal_promise_base
{
private:
  struct final_awaitable
  {
    bool
    await_ready() const noexcept
    {
      return false;
    }

    template <class Promise>
    std::coroutine_handle<>
    await_suspend( std::coroutine_handle<Promise> h ) noexcept
    {
      auto& tasks = h.promise().tasks_;
      auto continuation = h.promise().continuation_;

      auto cnt = tasks.erase( h.address() );
      (void)cnt;
      BOOST_ASSERT( cnt == 1 );

      // this means there are no living copies of the internal_task left
      // which means that there's no post_awaitable alive so we're the only ones
      // with visibility into this coroutine's frame
      BOOST_ASSERT( h.promise().count_ > 0 );
      if ( --h.promise().count_ == 0 ) {
        h.destroy();
      }

      if ( continuation ) {
        return continuation;
      }

      return std::noop_coroutine();
    }

    void
    await_resume() noexcept
    {
      BOOST_ASSERT( false );
    }
  };

public:
  promise_variant<T> variant_;
  task_map_type& tasks_;
  std::coroutine_handle<> continuation_ = nullptr;
  int count_ = 0;

  internal_promise_base() = delete;
  internal_promise_base( task_map_type& tasks ) : tasks_{ tasks } {}

  std::suspend_always
  initial_suspend()
  {
    return {};
  }
  final_awaitable
  final_suspend() noexcept
  {
    return {};
  }

  void
  unhandled_exception()
  {
    // current ref count + the ref count found in the corresponding awaitable
    auto const has_awaiter = ( count_ > 1 );
    if ( has_awaiter ) {
      variant_.set_error();
    } else {
      throw;
    }
  }
};

template <class T>
struct internal_promise : public internal_promise_base<T>
{
  template <class... Args>
  internal_promise( task_map_type& tasks, Args&&... )
      : internal_promise_base<T>( tasks )
  {
  }

  internal_task<T>
  get_return_object()
  {
    return internal_task<T>(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  template <class Expr>
  void
  return_value( Expr&& expr )
  {
    this->variant_.emplace( std::forward<Expr>( expr ) );
  }
};

template <>
struct internal_promise<void> : public internal_promise_base<void>
{
  template <class... Args>
  internal_promise( task_map_type& tasks, Args&&... )
      : internal_promise_base( tasks )
  {
  }

  internal_task<void>
  get_return_object()
  {
    return internal_task<void>(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  void
  return_void()
  {
  }
};

} // namespace detail

template <class T>
struct spawn_awaitable
{
  executor ex_;
  detail::internal_task<T> task_;
  bool was_awaited_ = false;

  spawn_awaitable( executor ex, detail::internal_task<T> task )
      : ex_{ ex }, task_{ task }
  {
  }

  ~spawn_awaitable()
  {
    auto& promise = task_.h_.promise();
    if ( promise.variant_.has_error() && !was_awaited_ ) {
      detail::executor_access_policy::unhandled_exception(
          ex_, promise.variant_.get_error() );
    }
  }

  bool
  await_ready() const noexcept
  {
    return false;
  }

  bool
  await_suspend( std::coroutine_handle<> awaiting_coroutine )
  {
    BOOST_ASSERT( task_.h_.promise().count_ > 0 );

    task_.h_.promise().continuation_ = awaiting_coroutine;

    bool const task_ended = ( task_.h_.promise().count_ < 2 );
    return !task_ended;
  }

  T
  await_resume()
  {
    was_awaited_ = true;
    return std::move( task_.h_.promise().variant_ ).result();
  }
};

namespace detail {

template <class T>
internal_task<T>
scheduler( detail::task_map_type& /* tasks */, task<T> t )
{
  co_return co_await t;
}
} // namespace detail

template <class T>
spawn_awaitable<T>
executor::spawn( task<T> t )
{
  auto internal_task = detail::scheduler( pframe_->tasks_, std::move( t ) );
  auto [it, b] = pframe_->tasks_.emplace( internal_task.h_,
                                          &internal_task.h_.promise().count_ );

  BOOST_ASSERT( b );
  pframe_->run_queue_.push_back( it->first );

  return { *this, internal_task };
}

void
executor::post( task<void> t ) const
{
  auto fd = pframe_->pipefd_[1];
  auto p = t.into_address();
  auto data = reinterpret_cast<std::uintptr_t>( p );
  data |= detail::post_mask;

  ssize_t ret = -1;
  ret = write( fd, &data, sizeof( data ) );
  if ( ret == -1 ) {
    detail::throw_errno_as_error_code( errno );
  }

  // do this to make tsan happy
  {
    auto guard = std::lock_guard<std::mutex>( pframe_->m_ );
  }
}

waker
executor::make_waker( std::coroutine_handle<> h ) const
{
  return detail::executor_access_policy::get_waker( *this, h );
}

template <class T>
spawn_awaitable<T>
spawn( executor ex, task<T> t )
{
  return ex.spawn( std::move( t ) );
}

template <class T>
void
post( executor ex, task<T> t )
{
  ex.post( std::move( t ) );
}

} // namespace fiona

#endif // FIONA_EXECUTOR_HPP
