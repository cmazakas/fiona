#ifndef FIONA_DETAIL_COMMON_HPP
#define FIONA_DETAIL_COMMON_HPP

// clang-format off
#include <fiona/params.hpp>                        // for io_context_params

#include <boost/container_hash/hash.hpp>           // for hash
#include <boost/unordered/unordered_flat_map.hpp>  // for unordered_flat_map
#include <boost/unordered/unordered_flat_set.hpp>  // for unordered_flat_set

#include <coroutine>                               // for coroutine_handle
#include <cstdint>                                 // for uint16_t
#include <cstring>                                 // for size_t
#include <deque>                                   // for deque
#include <exception>                               // for exception_ptr
#include <mutex>                                   // for mutex
#include <span>                                    // for span
#include <vector>                                  // for vector

#include <liburing.h>                              // for io_uring

struct io_uring_buf_ring;
// clang-format on

namespace fiona {
namespace detail {

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

  std::span<unsigned char> get_buffer_view( std::size_t buffer_id ) noexcept {
    return { bufs_[buffer_id] };
  }
  io_uring_buf_ring* get() const noexcept { return buf_ring_; }
  std::size_t size() const noexcept { return bufs_.size(); }
  std::uint16_t bgid() const noexcept { return bgid_; }
};

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

using task_map_type =
    boost::unordered_flat_map<std::coroutine_handle<>, int*, hasher, key_equal>;

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
} // namespace fiona

#endif // FIONA_DETAIL_COMMON_HPP
