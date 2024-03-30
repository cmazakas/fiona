#ifndef FIONA_DETAIL_COMMON_HPP
#define FIONA_DETAIL_COMMON_HPP

#include <fiona/buffer.hpp>                       // for recv_buffer
#include <fiona/detail/config.hpp>                // for FIONA_DECL
#include <fiona/params.hpp>                       // for io_context_params

#include <boost/container_hash/hash.hpp>          // for hash
#include <boost/unordered/unordered_flat_map.hpp> // for unordered_flat_map
#include <boost/unordered/unordered_flat_set.hpp> // for unordered_flat_set

#include <array>                                  // for array
#include <coroutine>                              // for coroutine_handle
#include <cstdint>                                // for uint16_t, uint32_t
#include <cstring>                                // for size_t
#include <deque>                                  // for deque
#include <exception>                              // for exception_ptr
#include <list>                                   // for list
#include <mutex>                                  // for mutex
#include <vector>                                 // for vector

#include <liburing.h>                             // for io_uring

struct io_uring_buf_ring;

namespace fiona {
namespace detail {

struct buf_ring {
public:
  std::vector<recv_buffer> bufs_;
  std::vector<std::size_t> buf_ids_;
  std::vector<std::size_t>::iterator buf_id_pos_;
  std::size_t buf_size_ = 0;
  io_uring* ring_ = nullptr;
  io_uring_buf_ring* buf_ring_ = nullptr;
  std::uint16_t bgid_ = 0;

  buf_ring() = delete;

  buf_ring( buf_ring const& ) = delete;
  buf_ring& operator=( buf_ring const& ) = delete;

  buf_ring( buf_ring&& rhs ) = delete;
  buf_ring& operator=( buf_ring&& rhs ) = delete;

  FIONA_DECL
  buf_ring( io_uring* ring, std::uint32_t num_bufs, std::uint16_t bgid );

  FIONA_DECL
  buf_ring( io_uring* ring, std::uint32_t num_bufs, std::size_t buf_size,
            std::uint16_t bgid );

  // TODO: this probably needs to be exported but we currently lack test
  // coverage for this
  ~buf_ring();

  recv_buffer& get_buf( std::size_t idx ) noexcept { return bufs_[idx]; }
  io_uring_buf_ring* get() const noexcept { return buf_ring_; }
  std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>( bufs_.size() );
  }
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
  std::list<buf_ring> buf_rings_;
  boost::unordered_flat_set<int> fds_;
  std::deque<std::coroutine_handle<>> run_queue_;
  std::exception_ptr exception_ptr_;
  std::array<int, 2> pipefd_ = { -1, -1 };

  FIONA_DECL
  io_context_frame( io_context_params const& io_ctx_params );

  FIONA_DECL
  ~io_context_frame();
};

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_COMMON_HPP
