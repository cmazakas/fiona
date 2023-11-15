#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

// clang-format off
#include <fiona/error.hpp>                         // for throw_errno_as_error_code
#include <fiona/task.hpp>                          // for task

#include <fiona/detail/awaitable_base.hpp>         // for intrusive_ptr_add_ref, awaitable_base, intrusive_ptr_release
#include <fiona/detail/get_sqe.hpp>                // for reserve_sqes

#include <boost/assert.hpp>                        // for BOOST_ASSERT
#include <boost/container_hash/hash.hpp>           // for hash
#include <boost/smart_ptr/intrusive_ptr.hpp>       // for intrusive_ptr
#include <boost/unordered/unordered_flat_map.hpp>  // for unordered_flat_map
#include <boost/unordered/unordered_flat_set.hpp>  // for unordered_flat_set

#include <coroutine>                               // for coroutine_handle, noop_coroutine, suspend_always
#include <cstdint>                                 // for uint32_t, uint16_t
#include <cstring>                                 // for size_t, memcpy
#include <deque>                                   // for deque
#include <exception>                               // for exception_ptr, rethrow_exception, current_exception
#include <memory>                                  // for shared_ptr, __shared_ptr_access, make_shared, weak_ptr
#include <mutex>                                   // for mutex, lock_guard
#include <span>                                    // for span
#include <utility>                                 // for move, addressof, forward, pair
#include <vector>                                  // for vector

#include <errno.h>                                 // for EINVAL, errno
#include <liburing.h>                              // for io_uring_get_sqe, io_uring_sqe_set_data, io_uring_prep_read
#include <liburing/io_uring.h>                     // for io_uring_cqe
#include <unistd.h>                                // for write

namespace fiona { namespace detail { struct executor_access_policy; } }
namespace fiona { namespace detail { struct pipe_awaitable; } }
namespace fiona { namespace detail { template <class T> struct internal_promise; } }
namespace fiona { template <class T> struct post_awaitable; }
// clang-format on

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
    return ( *this )( h.address() );
  }

  std::size_t operator()( void* p ) const noexcept {
    boost::hash<void*> hasher;
    return hasher( p );
  }
};

struct key_equal {
  using is_transparent = void;

  template <class Promise1, class Promise2>
  bool operator()( std::coroutine_handle<Promise1> const h1,
                   std::coroutine_handle<Promise2> const h2 ) const noexcept {
    return h1.address() == h2.address();
  }

  template <class Promise>
  bool operator()( std::coroutine_handle<Promise> const h,
                   void* p ) const noexcept {
    return h.address() == p;
  }

  template <class Promise>
  bool operator()( void* p,
                   std::coroutine_handle<Promise> const h ) const noexcept {
    return h.address() == p;
  }
};

namespace detail {

using task_map_type =
    boost::unordered_flat_map<std::coroutine_handle<>, int*, hasher, key_equal>;
} // namespace detail

struct waker {
  std::weak_ptr<void> p_;
  std::mutex& m_; // guess this dangles if the runtime dies; fix this
  int fd_ = -1;
  std::coroutine_handle<> h_;

  void wake() const {
    void const* const ptr = h_.address();

    auto p = p_.lock();
    if ( !p ) {
      detail::throw_errno_as_error_code( EINVAL );
    }

    std::lock_guard guard( m_ );

    int ret = -1;
    ret = write( fd_, &ptr, sizeof( ptr ) );
    if ( ret == -1 ) {
      detail::throw_errno_as_error_code( errno );
    }
  }
};

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
struct internal_task {
  using promise_type = internal_promise<T>;

  std::coroutine_handle<promise_type> h_ = nullptr;

  internal_task() = default;
  internal_task( std::coroutine_handle<promise_type> h ) : h_( h ) {
    BOOST_ASSERT( h_.promise().count_ == 0 );
    h_.promise().count_ = 2;
  }

  ~internal_task() {
    if ( h_ && --h_.promise().count_ == 0 ) {
      h_.destroy();
    }
  }

  internal_task( internal_task const& rhs ) {
    h_ = rhs.h_;
    ++h_.promise().count_;
  }

  internal_task& operator=( internal_task const& rhs ) {
    if ( this != &rhs ) {
      if ( --h_.promise().count_ == 0 ) {
        h_.destroy();
      }

      h_ = rhs.h_;
      ++h_.promise().count_;
    }
    return *this;
  }

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

  bool has_error() const noexcept { return rt_ == result_type::error; }

  std::exception_ptr get_error() const {
    BOOST_ASSERT( rt_ == result_type::error );
    return s_.exception_;
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

  bool has_error() const noexcept { return rt_ == result_type::error; }

  std::exception_ptr get_error() const {
    BOOST_ASSERT( rt_ == result_type::error );
    return exception_;
  }

  void result() {
    if ( exception_ ) {
      std::rethrow_exception( exception_ );
    }
  }
};

template <class T>
struct internal_promise_base {
private:
  struct final_awaitable {
    bool await_ready() const noexcept { return false; }

    template <class Promise>
    std::coroutine_handle<>
    await_suspend( std::coroutine_handle<Promise> h ) noexcept {
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

    void await_resume() noexcept { BOOST_ASSERT( false ); }
  };

public:
  promise_variant<T> variant_;
  task_map_type& tasks_;
  std::coroutine_handle<> continuation_ = nullptr;
  int count_ = 0;

  internal_promise_base() = delete;
  internal_promise_base( task_map_type& tasks ) : tasks_{ tasks } {}

  std::suspend_always initial_suspend() { return {}; }
  final_awaitable final_suspend() noexcept { return {}; }

  void unhandled_exception() {
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
struct internal_promise : public internal_promise_base<T> {
  template <class... Args>
  internal_promise( task_map_type& tasks, Args&&... )
      : internal_promise_base<T>( tasks ) {}

  internal_task<T> get_return_object() {
    return internal_task(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  template <class Expr>
  void return_value( Expr&& expr ) {
    this->variant_.emplace( std::forward<Expr>( expr ) );
  }
};

template <>
struct internal_promise<void> : public internal_promise_base<void> {
  template <class... Args>
  internal_promise( task_map_type& tasks, Args&&... )
      : internal_promise_base( tasks ) {}

  internal_task<void> get_return_object() {
    return internal_task(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  void return_void() {}
};

struct io_context_frame {
  io_uring io_ring_;
  std::mutex m_;
  task_map_type tasks_;
  io_context_params params_;
  std::vector<buf_ring> buf_rings_;
  boost::unordered_flat_set<int> fds_;
  std::deque<std::coroutine_handle<>> run_queue_;
  std::exception_ptr exception_ptr_;
  int pipefd_[2] = { -1, -1 };

  io_context_frame( io_context_params const& io_ctx_params );
  ~io_context_frame();
};

} // namespace detail

struct executor {
private:
  friend struct detail::executor_access_policy;

  std::shared_ptr<detail::io_context_frame> pframe_;

public:
  executor( std::shared_ptr<detail::io_context_frame> pframe ) noexcept
      : pframe_( std::move( pframe ) ) {}

  template <class T>
  post_awaitable<T> post( task<T> t );

  inline waker make_waker( std::coroutine_handle<> h );
};

namespace detail {

struct executor_access_policy {
  static inline io_uring* ring( executor ex ) noexcept {
    return &ex.pframe_->io_ring_;
  }

  static void unhandled_exception( executor ex, std::exception_ptr ep ) {
    if ( !ex.pframe_->exception_ptr_ ) {
      ex.pframe_->exception_ptr_ = ep;
    }
  }

  static inline int get_available_fd( executor ex ) {
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

  static inline void release_fd( executor ex, int fd ) {
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

  static inline buf_ring* get_buffer_group( executor ex,
                                            std::size_t bgid ) noexcept {
    for ( auto& bg : ex.pframe_->buf_rings_ ) {
      if ( bgid == bg.bgid() ) {
        return &bg;
      }
    }
    return nullptr;
  }

  static inline pipe_awaitable get_pipe_awaitable( executor ex );

  static inline waker get_waker( executor ex, std::coroutine_handle<> h ) {
    return { ex.pframe_, ex.pframe_->m_, ex.pframe_->pipefd_[1], h };
  }
};

} // namespace detail

template <class T>
struct post_awaitable {
  executor ex_;
  detail::internal_task<T> task_;
  bool was_awaited_ = false;

  post_awaitable( executor ex, detail::internal_task<T> task )
      : ex_{ ex }, task_{ task } {}

  ~post_awaitable() {
    auto& promise = task_.h_.promise();
    if ( promise.variant_.has_error() && !was_awaited_ ) {
      detail::executor_access_policy::unhandled_exception(
          ex_, promise.variant_.get_error() );
    }
  }

  bool await_ready() const noexcept { return false; }

  bool await_suspend( std::coroutine_handle<> awaiting_coroutine ) {
    BOOST_ASSERT( task_.h_.promise().count_ > 0 );

    task_.h_.promise().continuation_ = awaiting_coroutine;

    bool const task_ended = ( task_.h_.promise().count_ < 2 );
    return !task_ended;
  }

  T await_resume() {
    was_awaited_ = true;
    return std::move( task_.h_.promise().variant_ ).result();
  }
};

template <class T>
post_awaitable<T>
executor::post( task<T> t ) {
  auto ring = detail::executor_access_policy::ring( *this );

  auto internal_task = scheduler( pframe_->tasks_, ring, std::move( t ) );
  auto [it, b] = pframe_->tasks_.emplace( internal_task.h_,
                                          &internal_task.h_.promise().count_ );

  BOOST_ASSERT( b );
  pframe_->run_queue_.push_back( it->first );

  return { *this, internal_task };
}

template <class T>
detail::internal_task<T>
scheduler( detail::task_map_type& /* tasks */, io_uring* /* ring */,
           task<T> t ) {
  co_return co_await t;
}

struct io_context {
private:
  std::shared_ptr<detail::io_context_frame> pframe_;

public:
  io_context( io_context_params const& params = {} )
      : pframe_( std::make_shared<detail::io_context_frame>( params ) ) {}

  ~io_context();

  executor get_executor() const noexcept { return executor{ pframe_ }; }
  io_context_params params() const noexcept { return pframe_->params_; }

  void post( task<void> t ) {
    auto ex = get_executor();
    ex.post( std::move( t ) );
  }

  void register_buffer_sequence( std::size_t num_bufs, std::size_t buf_size,
                                 std::uint16_t buffer_group_id ) {
    auto ring = &pframe_->io_ring_;
    auto br = buf_ring( ring, num_bufs, buf_size, buffer_group_id );
    pframe_->buf_rings_.push_back( std::move( br ) );
  }

  void run();
};

template <class T>
post_awaitable<T>
post( executor ex, task<T> t ) {
  return ex.post( std::move( t ) );
}

namespace detail {

struct pipe_awaitable {
  struct frame : public detail::awaitable_base {
    executor ex_;
    char buffer_[sizeof( std::coroutine_handle<> )] = {};
    int fd_ = -1;
    int count_ = 0;

    frame( executor ex, int fd ) : ex_{ ex }, fd_{ fd } {}

    std::coroutine_handle<> h_ = nullptr;

    void init() {
      auto ring = detail::executor_access_policy::ring( ex_ );
      detail::reserve_sqes( ring, 1 );
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_read( sqe, fd_, buffer_, sizeof( buffer_ ), 0 );
      io_uring_sqe_set_data( sqe, this );
      intrusive_ptr_add_ref( this );
    }

    void await_process_cqe( io_uring_cqe* cqe ) override {
      if ( cqe->res != sizeof( void* ) ) {
        BOOST_ASSERT( cqe->res < 0 );
        detail::throw_errno_as_error_code( -cqe->res );
      }

      void* addr = nullptr;
      std::memcpy( &addr, buffer_, sizeof( addr ) );

      h_ = std::coroutine_handle<>::from_address( addr );

      {
        auto ring = detail::executor_access_policy::ring( ex_ );
        detail::reserve_sqes( ring, 1 );
        auto sqe = io_uring_get_sqe( ring );
        io_uring_prep_read( sqe, fd_, buffer_, sizeof( buffer_ ), 0 );
        io_uring_sqe_set_data( sqe, this );
        intrusive_ptr_add_ref( this );
      }
    }

    std::coroutine_handle<> handle() noexcept override {
      BOOST_ASSERT( h_ );
      return h_;
    }

    void inc_ref() noexcept override { ++count_; }
    void dec_ref() noexcept override {
      --count_;
      if ( count_ == 0 ) {
        delete this;
      }
    }

    int use_count() const noexcept override { return count_; }
  };

  boost::intrusive_ptr<frame> p_;

  pipe_awaitable( executor ex, int fd ) : p_( new frame( ex, fd ) ) {
    p_->init();
  }

  ~pipe_awaitable() { cancel(); }

  void cancel() {
    auto& self = *p_;
    auto ring = detail::executor_access_policy::ring( self.ex_ );
    detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_prep_cancel( sqe, p_.get(), 0 );
    io_uring_submit( ring );
  }
};

pipe_awaitable
executor_access_policy::get_pipe_awaitable( executor ex ) {
  return detail::pipe_awaitable( ex, ex.pframe_->pipefd_[0] );
}

} // namespace detail

waker
executor::make_waker( std::coroutine_handle<> h ) {
  return detail::executor_access_policy::get_waker( *this, h );
}

} // namespace fiona

#endif // FIONA_IO_CONTEXT_HPP
