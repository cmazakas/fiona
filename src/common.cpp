#include <fiona/buffer.hpp>        // for recv_buffer
#include <fiona/error.hpp>         // for throw_errno_as_error_code

#include <fiona/detail/common.hpp> // for buf_ring

#include <boost/assert.hpp>        // for BOOST_ASSERT

#include <cstddef>                 // for size_t
#include <cstdint>                 // for uint16_t, uint32_t
#include <vector>                  // for vector

#include <liburing.h>              // for io_uring_buf_ring_add, io_uring_b...

namespace fiona {
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
                         io_uring_buf_ring_mask( size() ), 0 );
  io_uring_buf_ring_advance( buf_ring_, 1 );
  b = std::move( buf );
}

} // namespace detail
} // namespace fiona
