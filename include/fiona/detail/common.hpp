// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_DETAIL_COMMON_HPP
#define FIONA_DETAIL_COMMON_HPP

#include <fiona/buffer.hpp>                       // for recv_buffer
#include <fiona/params.hpp>                       // for io_context_params

#include <boost/container_hash/hash.hpp>          // for hash
#include <boost/unordered/unordered_flat_map.hpp> // for unordered_flat_map
#include <boost/unordered/unordered_flat_set.hpp> // for unordered_flat_set
#include <boost/unordered/unordered_node_map.hpp>

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

struct buf_ring
{
public:
  std::vector<recv_buffer> bufs_;
  std::vector<std::size_t> buf_ids_;
  std::size_t* buf_id_pos_ = nullptr;
  std::size_t buf_size_ = 0;
  io_uring* ring_ = nullptr;
  io_uring_buf_ring* buf_ring_ = nullptr;
  std::uint16_t bgid_ = 0;

  buf_ring() = delete;

  buf_ring( buf_ring const& ) = delete;
  buf_ring& operator=( buf_ring const& ) = delete;

  FIONA_EXPORT
  buf_ring( io_uring* ring, std::uint32_t num_bufs, std::uint16_t bgid );

  FIONA_EXPORT
  buf_ring( io_uring* ring,
            std::uint32_t num_bufs,
            std::size_t buf_size,
            std::uint16_t bgid );

  FIONA_EXPORT
  ~buf_ring();

  recv_buffer&
  get_buf( std::size_t idx ) noexcept
  {
    auto& buf = bufs_[idx];
    return buf;
  }

  io_uring_buf_ring*
  as_ptr() const noexcept
  {
    return buf_ring_;
  }

  std::uint32_t
  size() const noexcept
  {
    return static_cast<std::uint32_t>( bufs_.size() );
  }

  std::uint16_t
  bgid() const noexcept
  {
    return bgid_;
  }

  FIONA_EXPORT
  void recycle_buffer( recv_buffer buf );
};

struct hasher
{
  using is_transparent = void;

  template <class Promise>
  std::size_t
  operator()( std::coroutine_handle<Promise> h ) const noexcept
  {
    return ( *this )( h.address() );
  }

  std::size_t
  operator()( void* p ) const noexcept
  {
    boost::hash<void*> hasher;
    return hasher( p );
  }
};

struct key_equal
{
  using is_transparent = void;

  template <class Promise1, class Promise2>
  bool
  operator()( std::coroutine_handle<Promise1> const h1,
              std::coroutine_handle<Promise2> const h2 ) const noexcept
  {
    return h1.address() == h2.address();
  }

  template <class Promise>
  bool
  operator()( std::coroutine_handle<Promise> const h, void* p ) const noexcept
  {
    return h.address() == p;
  }

  template <class Promise>
  bool
  operator()( void* p, std::coroutine_handle<Promise> const h ) const noexcept
  {
    return h.address() == p;
  }
};

struct task_map
{
  using map_type = boost::
      unordered_flat_map<std::coroutine_handle<>, int*, hasher, key_equal>;
  using iterator = typename map_type::iterator;

  map_type tasks_;

  task_map() = default;

  task_map( task_map const& ) = delete;
  task_map& operator=( task_map const& ) = delete;

  bool
  empty() const noexcept
  {
    return tasks_.empty();
  }

  template <class Promise>
  void
  add_task( std::coroutine_handle<Promise> h )
  {
    auto* p = &h.promise().count_;
    tasks_.emplace( h, p );
    *p += 1;
  }

  void
  erase_task( std::coroutine_handle<> h )
  {
    auto pos = tasks_.find( h );
    BOOST_ASSERT( pos != tasks_.end() );

    auto p_count = pos->second;
    BOOST_ASSERT( *p_count > 0 );

    if ( --*p_count == 0 ) {
      h.destroy();
    }
    tasks_.erase( pos );
  }

  void
  clear()
  {
    while ( !tasks_.empty() ) {
      auto pos = tasks_.begin();
      auto [h, p_count] = *pos;
      if ( --*p_count == 0 ) {
        h.destroy();
      }
      tasks_.erase( pos );
    }
  }
};

struct io_context_frame
{
  io_uring io_ring_;
  std::mutex m_;
  task_map tasks_;
  io_context_params params_;
  boost::unordered_node_map<std::uint16_t, buf_ring> buf_rings_;
  boost::unordered_flat_set<int> fds_;
  std::deque<std::coroutine_handle<>> run_queue_;
  std::exception_ptr exception_ptr_;
  std::array<int, 2> pipefd_ = { -1, -1 };

  FIONA_EXPORT
  io_context_frame( io_context_params const& io_ctx_params );

  FIONA_EXPORT
  ~io_context_frame();
};

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_COMMON_HPP
