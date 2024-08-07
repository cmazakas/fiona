// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <fiona/buffer.hpp>        // for recv_buffer
#include <fiona/error.hpp>         // for throw_errno_as_error_code

#include <fiona/detail/common.hpp> // for buf_ring

#include <boost/assert.hpp>        // for BOOST_ASSERT

#include <cstddef>                 // for size_t
#include <cstdint>                 // for uint16_t, uint32_t
#include <vector>                  // for vector

#include <liburing.h>              // for io_uring_buf_ring_add, io_uring_b...

namespace fiona {

fixed_buffers::fixed_buffers( io_uring* ring,
                              unsigned num_bufs,
                              std::size_t buf_size )
    : bufs_( num_bufs, std::vector<unsigned char>( buf_size, 0 ) ),
      iovecs_( num_bufs ), buf_ids_( num_bufs ), ring_( ring )
{
  for ( std::size_t i = 0; i < num_bufs; ++i ) {
    auto& buf = bufs_[i];
    iovecs_[i] = iovec{ .iov_base = buf.data(), .iov_len = buf.size() };
    buf_ids_[i] = i;
  }
  avail_buf_ids_ = buf_ids_.data() + num_bufs;

  io_uring_register_buffers( ring_, iovecs_.data(), num_bufs );
}

fixed_buffers::~fixed_buffers() { io_uring_unregister_buffers( ring_ ); }

//------------------------------------------------------------------------------

namespace detail {

buf_ring::buf_ring( io_uring* ring, std::uint32_t num_bufs, std::uint16_t bgid )
    : bufs_( num_bufs ), buf_ids_( num_bufs ), ring_( ring ), bgid_{ bgid }
{
  int ret = 0;

  auto* buf_ring = io_uring_setup_buf_ring( ring_, num_bufs, bgid, 0, &ret );
  if ( !buf_ring ) {
    throw_errno_as_error_code( -ret );
  }

  buf_ring_ = buf_ring;
  buf_id_pos_ = buf_ids_.data();
}

buf_ring::buf_ring( io_uring* ring,
                    std::uint32_t num_bufs,
                    std::size_t buf_size,
                    std::uint16_t bgid )
    : buf_ring( ring, num_bufs, bgid )
{
  buf_size_ = buf_size;
  for ( auto& buf : bufs_ ) {
    buf = recv_buffer( buf_size_ );
  }

  auto mask = io_uring_buf_ring_mask( static_cast<unsigned>( bufs_.size() ) );
  for ( std::size_t i = 0; i < bufs_.size(); ++i ) {
    auto& buf = bufs_[i];
    BOOST_ASSERT( buf.capacity() > 0 );
    io_uring_buf_ring_add(
        buf_ring_, buf.data(), static_cast<unsigned>( buf.capacity() ),
        static_cast<unsigned short>( i ), mask, static_cast<int>( i ) );
  }
  io_uring_buf_ring_advance( buf_ring_, static_cast<int>( bufs_.size() ) );
}

buf_ring::~buf_ring()
{
  if ( buf_ring_ ) {
    BOOST_ASSERT( ring_ );
    io_uring_free_buf_ring( ring_, buf_ring_,
                            static_cast<unsigned>( bufs_.size() ), bgid_ );
  }
}

void
buf_ring::recycle_buffer( recv_buffer buf )
{
  if ( buf_id_pos_ == buf_ids_.data() ) {
    return;
  }

  auto buffer_id = *( --buf_id_pos_ );
  auto& b = get_buf( buffer_id );
  BOOST_ASSERT( b.capacity() == 0 );

  BOOST_ASSERT( buf.capacity() > 0 );
  io_uring_buf_ring_add( buf_ring_, buf.data(),
                         static_cast<unsigned>( buf.capacity() ),
                         static_cast<unsigned short>( buffer_id ),
                         io_uring_buf_ring_mask( num_bufs() ), 0 );
  io_uring_buf_ring_advance( buf_ring_, 1 );
  b = std::move( buf );
}

} // namespace detail
} // namespace fiona
