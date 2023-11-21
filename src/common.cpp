// clang-format off

#include <fiona/error.hpp>          // for throw_errno_as_error_code
#include <fiona/detail/common.hpp>  // for buf_ring

#include <boost/assert.hpp>         // for BOOST_ASSERT

#include <cstdint>                  // for uint32_t, uintptr_t, uint16_t
#include <cstdlib>                  // for free, size_t
#include <new>                      // for bad_alloc
#include <utility>                  // for move
#include <vector>                   // for vector

#include <liburing.h>               // for io_uring_unregister_buf_ring, io_uring_buf_ring_add, io_uring_buf_ring_ad...
#include <liburing/io_uring.h>      // for io_uring_buf, io_uring_buf_reg, io_uring_buf_ring (ptr only)
#include <stdlib.h>                 // for posix_memalign
#include <unistd.h>                 // for sysconf, _SC_PAGESIZE

// clang-format on

namespace fiona {
namespace detail {

buf_ring::buf_ring( io_uring* ring, std::size_t num_bufs, std::size_t buf_size,
                    std::uint16_t bgid )
    : bufs_( num_bufs ), ring_( ring ), bgid_{ bgid } {

  struct posix_memalign_guard {
    void* p = nullptr;
    ~posix_memalign_guard() {
      if ( p ) {
        free( p );
      }
    }
  };

  int ret = -1;

  posix_memalign_guard guard;

  std::size_t n = sizeof( io_uring_buf ) * bufs_.size();
  std::size_t pagesize = sysconf( _SC_PAGESIZE );
  ret = posix_memalign( &guard.p, pagesize, n );
  if ( ret != 0 ) {
    throw std::bad_alloc();
  }

  for ( auto& buf : bufs_ ) {
    buf.resize( buf_size );
  }

  buf_ring_ = static_cast<io_uring_buf_ring*>( guard.p );
  io_uring_buf_ring_init( buf_ring_ );

  io_uring_buf_reg reg = {
      .ring_addr = reinterpret_cast<std::uintptr_t>( buf_ring_ ),
      .ring_entries = static_cast<std::uint32_t>( bufs_.size() ),
      .bgid = bgid_,
      .flags = 0,
      .resv = { 0 },
  };

  ret = io_uring_register_buf_ring( ring_, &reg, 0 );
  if ( ret != 0 ) {
    fiona::detail::throw_errno_as_error_code( -ret );
  }

  for ( std::size_t i = 0; i < bufs_.size(); ++i ) {
    auto& buf = bufs_[i];
    io_uring_buf_ring_add( buf_ring_, buf.data(), buf.size(), i,
                           io_uring_buf_ring_mask( bufs_.size() ), i );
  }
  io_uring_buf_ring_advance( buf_ring_, bufs_.size() );

  guard.p = nullptr;
}

buf_ring::~buf_ring() {
  if ( buf_ring_ ) {
    BOOST_ASSERT( ring_ );
    io_uring_unregister_buf_ring( ring_, bgid_ );
    free( buf_ring_ );
  }
}

buf_ring::buf_ring( buf_ring&& rhs ) noexcept {
  bufs_ = std::move( rhs.bufs_ );
  ring_ = rhs.ring_;
  buf_ring_ = rhs.buf_ring_;
  bgid_ = rhs.bgid_;

  rhs.ring_ = nullptr;
  rhs.buf_ring_ = nullptr;
  rhs.bgid_ = 0;
}

buf_ring&
buf_ring::operator=( buf_ring&& rhs ) noexcept {
  if ( this != &rhs ) {
    if ( buf_ring_ ) {
      BOOST_ASSERT( ring_ );
      io_uring_unregister_buf_ring( ring_, bgid_ );
      free( buf_ring_ );
    }

    bufs_ = std::move( rhs.bufs_ );
    ring_ = rhs.ring_;
    buf_ring_ = rhs.buf_ring_;
    bgid_ = rhs.bgid_;

    rhs.ring_ = nullptr;
    rhs.buf_ring_ = nullptr;
    rhs.bgid_ = 0;
  }
  return *this;
}
} // namespace detail
} // namespace fiona
