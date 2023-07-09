#ifndef FIONA_TCP_HPP
#define FIONA_TCP_HPP

#include <fiona/detail/awaitable_base.hpp>
#include <fiona/error.hpp>
#include <fiona/io_context.hpp>

#include <boost/assert.hpp>

#include <cstddef>
#include <deque>
#include <iostream>

#include <liburing.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>

namespace fiona {
namespace tcp {

struct acceptor {
private:
  struct acceptor_awaitable final : fiona::detail::awaitable_base {
    friend struct acceptor;

  private:
    sockaddr_storage addr_storage_;
    socklen_t addr_storage_size_ = sizeof( addr_storage_ );
    std::deque<int> connections_;
    std::coroutine_handle<> h_;
    io_uring* ring_ = nullptr;
    int fd_ = -1;
    bool initiated_ = false;

    acceptor_awaitable( io_uring* ring, int fd ) : ring_( ring ), fd_{ fd } {}

  public:
    ~acceptor_awaitable() {
      if ( initiated_ ) {
        auto ring = ring_;
        auto sqe = io_uring_get_sqe( ring );
        io_uring_prep_cancel( sqe, this, 0 );
        sqe->user_data = reinterpret_cast<std::uintptr_t>( this );
        io_uring_submit( ring );
      }
    }

    void await_process_cqe( io_uring_cqe* cqe ) {
      auto fd = cqe->res;
      connections_.push_back( fd );
      if ( !( cqe->flags & IORING_CQE_F_MORE ) ) {
        initiated_ = false;
      }
    }

    std::coroutine_handle<> handle() noexcept {
      auto h = h_;
      h_ = nullptr;
      return h;
    }

    bool await_ready() noexcept { return !connections_.empty(); }
    void await_suspend( std::coroutine_handle<> h ) {
      h_ = h;
      if ( !initiated_ ) {
        auto ring = ring_;
        auto sqe = io_uring_get_sqe( ring );
        io_uring_prep_multishot_accept(
            sqe, fd_, reinterpret_cast<sockaddr*>( &addr_storage_ ),
            &addr_storage_size_, 0 );
        sqe->user_data = reinterpret_cast<std::uintptr_t>( this );
        io_uring_submit( ring );
        initiated_ = true;
      }
    }

    fiona::result<int> await_resume() {
      BOOST_ASSERT( !connections_.empty() );
      auto fd = connections_.front();
      connections_.pop_front();
      return fd;
    }
  };

private:
  fiona::executor ex_;
  int fd_ = -1;

public:
  acceptor( fiona::executor ex, std::uint32_t ipv4_addr, std::uint16_t port )
      : ex_( ex ) {
    int fd = socket( AF_INET, SOCK_STREAM, 0 );
    if ( fd == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    int enable = 1;
    if ( -1 == setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                           sizeof( enable ) ) ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    sockaddr_in addr;
    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_port = htons( port );
    addr.sin_addr.s_addr = ntohl( ipv4_addr );

    if ( -1 ==
         bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    constexpr int backlog = 256;
    if ( -1 == listen( fd, backlog ) ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    fd_ = fd;
  }

  ~acceptor() { close( fd_ ); }

  acceptor_awaitable async_accept() { return { ex_.ring(), fd_ }; }
};

struct client {
private:
  struct connect_awaitable final : public fiona::detail::awaitable_base {
    friend struct client;

  private:
    sockaddr_storage addr_;
    io_uring* ring_ = nullptr;
    std::coroutine_handle<> h_;
    int fd_ = -1;
    int res_ = 0;
    bool initiated_ = false;
    bool done_ = false;

    connect_awaitable( sockaddr_in ipv4_addr, io_uring* ring, int fd )
        : ring_( ring ), fd_{ fd } {
      memset( &addr_, 0, sizeof( addr_ ) );
      memcpy( &addr_, &ipv4_addr, sizeof( ipv4_addr ) );
    }

  public:
    ~connect_awaitable() {
      if ( initiated_ && !done_ ) {
        auto ring = ring_;
        auto sqe = io_uring_get_sqe( ring );
        io_uring_prep_cancel( sqe, this, 0 );
        sqe->user_data = reinterpret_cast<std::uintptr_t>( this );
        io_uring_submit( ring );
      }
    }

    bool await_ready() { return false; }
    void await_suspend( std::coroutine_handle<> h ) {
      if ( !initiated_ ) {
        auto ring = ring_;
        auto sqe = io_uring_get_sqe( ring );
        io_uring_prep_connect( sqe, fd_,
                               reinterpret_cast<sockaddr const*>( &addr_ ),
                               sizeof( addr_ ) );
        sqe->user_data = reinterpret_cast<std::uintptr_t>( this );
        io_uring_submit( ring );

        h_ = h;
        initiated_ = true;
      }
    }

    fiona::error_code await_resume() {
      if ( res_ == 0 ) {
        return {};
      }

      return fiona::error_code::from_errno( -res_ );
    }

    void await_process_cqe( io_uring_cqe* cqe ) {
      res_ = cqe->res;
      done_ = true;
    }

    std::coroutine_handle<> handle() noexcept { return h_; }
  };

private:
  fiona::executor ex_;
  int fd_ = -1;

public:
  client( fiona::executor ex ) : ex_( ex ) {
    int fd = socket( AF_INET, SOCK_STREAM, 0 );
    if ( fd == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    fd_ = fd;
  }

  ~client() { close( fd_ ); }

  connect_awaitable async_connect( std::uint32_t ipv4_addr,
                                   std::uint16_t port ) {

    sockaddr_in addr;
    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_port = htons( port );
    addr.sin_addr.s_addr = htonl( ipv4_addr );
    return { addr, ex_.ring(), fd_ };
  }
};

} // namespace tcp
} // namespace fiona

#endif // FIONA_TCP_HPP
