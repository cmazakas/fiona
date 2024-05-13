#include "fiona/buffer.hpp"
#include <fiona/detail/common.hpp>
#include <fiona/error.hpp>  // for throw_errno_as_error_code

#include <boost/assert.hpp> // for BOOST_ASSERT

#include <cstdint>          // for uint32_t, uintptr_t, uint16_t
#include <new>              // for bad_alloc
#include <optional>
#include <vector>           // for vector

#include <liburing.h> // for io_uring_buf_ring_add, io_uring_buf_ring_advance, io_uring_buf_ring_init, io_...
#include <liburing/io_uring.h> // for io_uring_buf, io_uring_buf_reg
#include <mm_malloc.h>         // for posix_memalign
#include <stdlib.h>            // for free
#include <unistd.h>            // for sysconf, _SC_PAGESIZE

namespace fiona {
namespace detail {

buf_ring::buf_ring( io_uring* ring, std::uint32_t num_bufs, std::uint16_t bgid )
    : bufs_( num_bufs ), buf_ids_( num_bufs ), ring_( ring ), bgid_{ bgid } {
  int ret = 0;

  auto* buf_ring = io_uring_setup_buf_ring( ring_, num_bufs, bgid, 0, &ret );
  if ( buf_ring == nullptr ) {
    throw_errno_as_error_code( -ret );
  }

  buf_ring_ = buf_ring;
  buf_id_pos_ = buf_ids_.begin();
}

buf_ring::buf_ring( io_uring* ring, std::uint32_t num_bufs,
                    std::size_t buf_size, std::uint16_t bgid )
    : buf_ring( ring, num_bufs, bgid ) {
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

buf_ring::~buf_ring() {
  if ( buf_ring_ ) {
    BOOST_ASSERT( ring_ );
    io_uring_free_buf_ring( ring_, buf_ring_,
                            static_cast<unsigned>( bufs_.size() ), bgid_ );
  }
}

} // namespace detail
} // namespace fiona
