#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

#include <fiona/task.hpp>

#include <boost/assert.hpp>
#include <boost/container_hash/hash.hpp>
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

struct internal_promise;

struct internal_task {
  using promise_type = internal_promise;

  std::coroutine_handle<promise_type> h_;

  internal_task( std::coroutine_handle<promise_type> h ) : h_( h ) {}
  ~internal_task() {
    if ( h_ ) {
      h_.destroy();
    }
  }

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

  friend std::size_t hash_value( internal_task const& t ) noexcept {
    boost::hash<void*> hasher;
    return hasher( t.h_.address() );
  }

  friend bool operator==( internal_task const& lhs, internal_task const& rhs ) {
    return lhs.h_.address() == rhs.h_.address();
  }
};

struct hasher {
  using is_transparent = void;

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

  bool operator()( internal_task const& t, void* p ) const noexcept {
    return t.h_.address() == p;
  }

  bool operator()( void* p, internal_task const& t ) const noexcept {
    return t.h_.address() == p;
  }
};

using task_set_type =
    boost::unordered_node_set<internal_task, hasher, key_equal>;

struct internal_promise {
  struct internal_final_awaitable {
    task_set_type& tasks;

    bool await_ready() const noexcept { return false; }
    void await_suspend( std::coroutine_handle<> h ) noexcept {
      tasks.erase( h.address() );
    }

    void await_resume() noexcept { BOOST_ASSERT( false ); }
  };

  template <class... Args>
  internal_promise( task_set_type& tasks_, Args&&... ) : tasks( tasks_ ) {}

  internal_task get_return_object() {
    return internal_task(
        std::coroutine_handle<internal_promise>::from_promise( *this ) );
  }

  std::suspend_always initial_suspend() { return {}; }
  internal_final_awaitable final_suspend() noexcept { return { tasks }; }

  void unhandled_exception() {
    std::rethrow_exception( std::current_exception() );
  }

  void return_void() {}

  task_set_type& tasks;
};

struct io_context_frame {
  io_uring io_ring_;
  task_set_type tasks_;
  io_context_params params_;
  std::vector<buf_ring> buf_rings_;
  boost::unordered_flat_set<int> fds_;
  std::deque<internal_task*> run_queue_;

  io_context_frame( io_context_params const& io_ctx_params );
  ~io_context_frame();
};

} // namespace detail

namespace detail {
struct executor_access_policy;
} // namespace detail

struct executor {
private:
  friend struct detail::executor_access_policy;

  boost::local_shared_ptr<detail::io_context_frame> framep_;

public:
  executor( boost::local_shared_ptr<detail::io_context_frame> framep ) noexcept
      : framep_( std::move( framep ) ) {}

  void post( task<void> t );
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

inline detail::internal_task
scheduler( detail::task_set_type& /* tasks */, io_uring* /* ring */,
           task<void> t ) {
  co_await t;
  co_return;
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
