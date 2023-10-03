#ifndef FIONA_BORROWED_BUFFER_HPP
#define FIONA_BORROWED_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <liburing.h>

namespace fiona {

struct borrowed_buffer {
private:
  io_uring_buf_ring* br_ = nullptr;
  void* addr_ = nullptr;
  unsigned len_ = 0;
  unsigned num_read_ = 0;
  std::size_t num_bufs_ = 0;
  std::uint16_t bid_ = 0;

public:
  borrowed_buffer() = default;

  borrowed_buffer( borrowed_buffer const& ) = delete;
  borrowed_buffer& operator=( borrowed_buffer const& ) = delete;

  borrowed_buffer( io_uring_buf_ring* br, void* addr, unsigned len,
                   std::size_t num_bufs, std::uint16_t bid, unsigned num_read )
      : br_( br ), addr_( addr ), len_{ len }, num_read_{ num_read },
        num_bufs_{ num_bufs }, bid_{ bid } {}

  borrowed_buffer( borrowed_buffer&& rhs ) noexcept
      : br_( rhs.br_ ),
        addr_( rhs.addr_ ), len_{ rhs.len_ }, num_read_{ rhs.num_read_ },
        num_bufs_{ rhs.num_bufs_ }, bid_{ rhs.bid_ } {
    rhs.br_ = nullptr;
    rhs.addr_ = nullptr;
    rhs.len_ = 0;
    rhs.num_read_ = 0;
    rhs.num_bufs_ = 0;
    rhs.bid_ = 0;
  }

  ~borrowed_buffer() {
    if ( br_ ) {
      auto buf_ring = br_;
      io_uring_buf_ring_add( buf_ring, addr_, len_, bid_,
                             io_uring_buf_ring_mask( num_bufs_ ), 0 );
      io_uring_buf_ring_advance( buf_ring, 1 );
    }
  }

  borrowed_buffer& operator=( borrowed_buffer&& rhs ) noexcept {
    if ( this != &rhs ) {
      if ( br_ ) {
        auto buf_ring = br_;
        io_uring_buf_ring_add( buf_ring, addr_, len_, bid_,
                               io_uring_buf_ring_mask( num_bufs_ ), 0 );
        io_uring_buf_ring_advance( buf_ring, 1 );
      }

      br_ = rhs.br_;
      addr_ = rhs.addr_;
      len_ = rhs.len_;
      num_read_ = rhs.num_read_;
      num_bufs_ = rhs.num_bufs_;
      bid_ = rhs.bid_;

      rhs.br_ = nullptr;
      rhs.addr_ = nullptr;
      rhs.len_ = 0;
      rhs.num_read_ = 0;
      rhs.num_bufs_ = 0;
      rhs.bid_ = 0;
    }
    return *this;
  }

  std::span<unsigned char> readable_bytes() const noexcept {
    return { static_cast<unsigned char*>( addr_ ), num_read_ };
  }

  std::string_view as_str() const noexcept {
    return { static_cast<char*>( addr_ ), num_read_ };
  }
};
} // namespace fiona

#endif // FIONA_BORROWED_BUFFER_HPP
