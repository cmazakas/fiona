#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

#include <fiona/task.hpp>

#include <boost/assert.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <boost/unordered/unordered_node_set.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <span>

#include <liburing.h>

namespace fiona {

struct io_context_params {
  std::uint32_t sq_entries = 4096;
  std::uint32_t cq_entries = 4096;
  std::uint32_t num_files = 1024;
};

using buffer_sequence_type = std::vector<std::vector<unsigned char>>;

struct hasher {
  using is_transparent = void;

  template <class Promise>
  std::size_t operator()( std::coroutine_handle<Promise> h ) const noexcept {
    boost::hash<void*> hasher;
    return hasher( h.address() );
  }

  template <class T>
  std::size_t operator()( T const& t ) const noexcept {
    boost::hash<T> hasher;
    return hasher( t );
  }
};

struct key_equal {
  using is_transparent = void;

  template <class T, class U>
  bool operator()( T const& t, U const& u ) const {
    return t == u;
  }

  bool operator()( std::coroutine_handle<> h, void* p ) const noexcept {
    return h.address() == p;
  }

  bool operator()( void* p, std::coroutine_handle<> h ) const noexcept {
    return h.address() == p;
  }
};

namespace detail {
using task_set_type =
    boost::unordered_flat_set<std::coroutine_handle<>, hasher, key_equal>;
}

struct buf_ring {
private:
  buffer_sequence_type bufs_;
  io_uring* ring_ = nullptr;
  io_uring_buf_ring* buf_ring_ = nullptr;
  std::uint16_t bgid_ = 0;

public:
  buf_ring() = delete;

  buf_ring( buf_ring const& ) = delete;
  buf_ring& operator=( buf_ring const& ) = delete;

  buf_ring( io_uring* ring, std::size_t num_bufs, std::size_t buf_size,
            std::uint16_t bgid );

  ~buf_ring();
  buf_ring( buf_ring&& rhs ) noexcept;
  buf_ring& operator=( buf_ring&& rhs ) noexcept;

  std::span<unsigned char> get_buffer( std::size_t bid ) noexcept {
    return { bufs_[bid] };
  }
  io_uring_buf_ring* get() const noexcept { return buf_ring_; }
  std::size_t size() const noexcept { return bufs_.size(); }
  std::uint16_t bgid() const noexcept { return bgid_; }
};

namespace detail {

template <class T>
struct internal_promise;

template <class T>
struct internal_task {
  using promise_type = internal_promise<T>;

  std::coroutine_handle<promise_type> h_;

  internal_task( std::coroutine_handle<promise_type> h ) : h_( h ) {}
  ~internal_task() {}

  internal_task( internal_task const& ) = delete;
  internal_task& operator=( internal_task const& ) = delete;

  internal_task( internal_task&& rhs ) noexcept : h_( rhs.h_ ) {
    rhs.h_ = nullptr;
  }

  internal_task& operator=( internal_task&& rhs ) noexcept {
    if ( this != &rhs ) {
      auto h = h_;
      h_ = rhs.h_;
      rhs.h_ = h;
    }
    return *this;
  }
};

template <class T>
struct promise_variant {
  enum class result_type { uninitialized, ok, error };

  union storage {
    T value_;
    std::exception_ptr exception_;

    storage() {}
    ~storage() {}
  };

  storage s_;
  result_type rt_ = result_type::uninitialized;

  ~promise_variant() {
    if ( rt_ == result_type::ok ) {
      s_.value_.~T();
    }

    if ( rt_ == result_type::error ) {
      s_.exception_.~exception_ptr();
    }
  }

  template <class... Args>
  void emplace( Args&&... args ) {
    BOOST_ASSERT( rt_ == result_type::uninitialized );
    new ( std::addressof( s_.value_ ) ) T( std::forward<Args>( args )... );
    rt_ = result_type::ok;
  }

  void set_error() {
    new ( std::addressof( s_.exception_ ) )
        std::exception_ptr( std::current_exception() );
    rt_ = result_type::error;
  }

  T& result() & {
    if ( rt_ == result_type::error ) {
      std::rethrow_exception( s_.exception_ );
    }
    return s_.value_;
  }

  T&& result() && {
    if ( rt_ == result_type::error ) {
      std::rethrow_exception( s_.exception_ );
    }
    return std::move( s_.value_ );
  }
};

template <>
struct promise_variant<void> {
  enum class result_type { uninitialized, ok, error };

  std::exception_ptr exception_;
  result_type rt_ = result_type::uninitialized;

  template <class... Args>
  void emplace( Args&&... ) {}

  void set_error() {
    exception_ = std::exception_ptr( std::current_exception() );
    rt_ = result_type::error;
  }

  void result() {
    if ( exception_ ) {
      std::rethrow_exception( exception_ );
    }
  }
};

template <class T>
struct internal_state
    : boost::intrusive_ref_counter<internal_state<T>,
                                   boost::thread_unsafe_counter> {
  promise_variant<T> variant_;
  std::coroutine_handle<> continuation_ = nullptr;
};

template <class T>
struct internal_promise_base {
private:
  struct final_awaitable {
    task_set_type& tasks;
    boost::intrusive_ptr<internal_state<T>> ps_;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<>
    await_suspend( std::coroutine_handle<> h ) noexcept {
      auto continuation = ps_->continuation_;

      tasks.erase( h.address() );
      h.destroy();

      if ( continuation ) {
        return continuation;
      }
      return std::noop_coroutine();
    }

    void await_resume() noexcept { BOOST_ASSERT( false ); }
  };

protected:
  task_set_type& tasks_;
  boost::intrusive_ptr<internal_state<T>> ps_ = nullptr;

public:
  internal_promise_base() = delete;
  internal_promise_base( task_set_type& tasks )
      : tasks_( tasks ), ps_( new internal_state<T>() ) {}

  std::suspend_always initial_suspend() { return {}; }
  final_awaitable final_suspend() noexcept { return { tasks_, ps_ }; }

  boost::intrusive_ptr<internal_state<T>> get_state() const noexcept {
    return ps_;
  }
};

template <class T>
struct internal_promise : public internal_promise_base<T> {
  template <class... Args>
  internal_promise( task_set_type& tasks, Args&&... )
      : internal_promise_base<T>( tasks ) {}

  internal_task<T> get_return_object() {
    return internal_task(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  void unhandled_exception() { this->ps_->variant_.set_error(); }

  template <class Expr>
  void return_value( Expr&& expr ) {
    this->ps_->variant_.emplace( std::forward<Expr>( expr ) );
  }
};

template <>
struct internal_promise<void> : public internal_promise_base<void> {
  template <class... Args>
  internal_promise( task_set_type& tasks, Args&&... )
      : internal_promise_base( tasks ) {}

  internal_task<void> get_return_object() {
    return internal_task(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  void unhandled_exception() { ps_->variant_.set_error(); }
  void return_void() {}
};

struct io_context_frame {
  io_uring io_ring_;
  task_set_type tasks_;
  io_context_params params_;
  std::vector<buf_ring> buf_rings_;
  boost::unordered_flat_set<int> fds_;
  std::deque<std::coroutine_handle<>> run_queue_;

  io_context_frame( io_context_params const& io_ctx_params );
  ~io_context_frame();
};

} // namespace detail

namespace detail {
struct executor_access_policy;
} // namespace detail

template <class T>
struct post_awaitable;

struct executor {
private:
  friend struct detail::executor_access_policy;

  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  executor( boost::local_shared_ptr<detail::io_context_frame> framep ) noexcept
      : framep_( std::move( framep ) ) {}

  template <class T>
  post_awaitable<T> post( task<T> t );
};

namespace detail {

struct executor_access_policy {
  static inline io_uring* ring( executor ex ) noexcept {
    return &ex.framep_->io_ring_;
  }

  static inline int get_available_fd( executor ex ) {
    auto& fds = ex.framep_->fds_;
    if ( fds.empty() ) {
      return -1;
    }
    auto pos = fds.begin();
    auto fd = *pos;
    fds.erase( pos );

    BOOST_ASSERT( fd >= 0 );
    BOOST_ASSERT( static_cast<std::uint32_t>( fd ) <
                  ex.framep_->params_.num_files );
    return fd;
  }

  static inline void release_fd( executor ex, int fd ) {
    if ( fd < 0 ) {
      return;
    }

    BOOST_ASSERT( static_cast<std::uint32_t>( fd ) <
                  ex.framep_->params_.num_files );

    auto& fds = ex.framep_->fds_;
    auto itb = fds.insert( fd );
    (void)itb;
    BOOST_ASSERT( itb.second );
  }

  static inline buf_ring* get_buffer_group( executor ex,
                                            std::size_t bgid ) noexcept {
    for ( auto& bg : ex.framep_->buf_rings_ ) {
      if ( bgid == bg.bgid() ) {
        return &bg;
      }
    }
    return nullptr;
  }
};

} // namespace detail

template <class T>
struct post_awaitable {
  boost::intrusive_ptr<detail::internal_state<T>> ps_;

  bool await_ready() const noexcept { return false; }

  bool await_suspend( std::coroutine_handle<> awaiting_coroutine ) {
    BOOST_ASSERT( ps_->use_count() > 0 );

    ps_->continuation_ = awaiting_coroutine;

    bool const task_ended = ( ps_->use_count() < 2 );
    return !task_ended;
  }

  T await_resume() { return this->ps_->variant_.result(); }
};

template <class T>
post_awaitable<T>
executor::post( task<T> t ) {
  auto ring = detail::executor_access_policy::ring( *this );

  auto internal_task = scheduler( framep_->tasks_, ring, std::move( t ) );
  auto [it, b] = framep_->tasks_.insert( internal_task.h_ );

  BOOST_ASSERT( b );
  framep_->run_queue_.push_back( *it );

  return { internal_task.h_.promise().get_state() };
}

template <class T>
detail::internal_task<T>
scheduler( detail::task_set_type& /* tasks */, io_uring* /* ring */,
           task<T> t ) {
  co_return co_await t;
}

struct io_context {
private:
  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  io_context( io_context_params const& params = {} )
      : framep_(
            boost::make_local_shared<detail::io_context_frame>( params ) ) {}

  executor get_executor() const noexcept { return executor{ framep_ }; }
  io_context_params params() const noexcept { return framep_->params_; }

  void post( task<void> t ) {
    auto ex = get_executor();
    ex.post( std::move( t ) );
  }

  void register_buffer_sequence( std::size_t num_bufs, std::size_t buf_size,
                                 std::uint16_t buffer_group_id ) {
    auto ring = &framep_->io_ring_;
    auto br = buf_ring( ring, num_bufs, buf_size, buffer_group_id );
    framep_->buf_rings_.push_back( std::move( br ) );
  }

  void run();
};

} // namespace fiona

#endif // FIONA_IO_CONTEXT_HPP
