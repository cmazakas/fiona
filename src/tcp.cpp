#include <fiona/tcp.hpp>

namespace fiona {
namespace tcp {

stream::~stream() {
  if ( fd_ >= 0 ) {
    auto ring = detail::executor_access_policy::ring( ex_ );
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_close_direct( sqe, fd_ );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_submit( ring );

    detail::executor_access_policy::release_fd( ex_, fd_ );
  }
}

stream&
stream::operator=( stream&& rhs ) noexcept {
  if ( this != &rhs ) {
    if ( fd_ >= 0 ) {
      auto ring = detail::executor_access_policy::ring( ex_ );
      auto sqe = fiona::detail::get_sqe( ring );
      io_uring_prep_close_direct( sqe, fd_ );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_submit( ring );

      detail::executor_access_policy::release_fd( ex_, fd_ );
    }

    ex_ = std::move( rhs.ex_ );
    fd_ = rhs.fd_;
    ts_ = rhs.ts_;

    rhs.fd_ = -1;
    rhs.ts_ = {};
  }
  return *this;
}

void
stream::recv_awaitable::frame::await_process_cqe( io_uring_cqe* cqe ) {
  if ( cqe->res >= 0 ) {
    if ( cqe->flags & IORING_CQE_F_BUFFER ) {
      std::uint16_t bid = cqe->flags >> 16;
      auto buf = br_.get_buffer( bid );
      buffers_.push_back( borrowed_buffer( br_.get(), buf.data(), buf.size(),
                                           br_.size(), bid, cqe->res ) );
    } else {
      // tcp connection hard-closed by peer
      BOOST_ASSERT( cqe->res == 0 );
      buffers_.push_back( borrowed_buffer() );
      initiated_ = false;
    }
  } else {
    buffers_.push_back( fiona::error_code::from_errno( -cqe->res ) );
  }

  if ( !( cqe->flags & IORING_CQE_F_MORE ) ) {
    initiated_ = false;
  }

  if ( ( cqe->flags & IORING_CQE_F_MORE ) ) {
    intrusive_ptr_add_ref( this );
  }
}

stream::recv_awaitable::~recv_awaitable() {
  auto& self = *p_;
  if ( self.initiated_ ) {
    auto ring = detail::executor_access_policy::ring( self.ex_ );
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, p_.get(), 0 );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_submit( ring );
  }
}

void
stream::recv_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& self = *p_;
  self.h_ = h;
  if ( self.initiated_ ) {
    return;
  }

  auto ring = detail::executor_access_policy::ring( self.ex_ );
  auto sqe = fiona::detail::get_sqe( ring );
  io_uring_prep_recv_multishot( sqe, self.fd_, nullptr, 0, 0 );
  io_uring_sqe_set_flags( sqe, IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE );
  io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
  sqe->buf_group = self.bgid_;

  self.initiated_ = true;
}

result<borrowed_buffer>
stream::recv_awaitable::await_resume() {
  auto& self = *p_;
  BOOST_ASSERT( !self.buffers_.empty() );
  auto borrowed_buf = std::move( self.buffers_.front() );
  self.buffers_.pop_front();

  return borrowed_buf;
}

stream::write_awaitable::~write_awaitable() {
  auto& self = *p_;
  if ( self.initiated_ && !self.done_ ) {
    auto ring = self.ring_;
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, p_.get(), 0 );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_submit( ring );
  }
}

void
stream::write_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& self = *p_;
  if ( self.initiated_ ) {
    return;
  }

  auto ring = self.ring_;

  if ( io_uring_sq_space_left( ring ) < 2 ) {
    io_uring_submit( ring );
  }

  {
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_send( sqe, self.fd_, self.buf_, self.nbytes_, MSG_WAITALL );
    io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
    io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK | IOSQE_FIXED_FILE );
  }

  {
    auto sqe = fiona::detail::get_sqe( ring );

    io_uring_prep_link_timeout( sqe, &self.ts_, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
  }

  self.h_ = h;
  self.initiated_ = true;
}

void
acceptor::accept_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& self = *p_;
  if ( self.initiated_ ) {
    return;
  }

  auto ring = self.ring_;
  auto sqe = fiona::detail::get_sqe( ring );
  auto file_idx = detail::executor_access_policy::get_available_fd( self.ex_ );

  io_uring_prep_accept_direct( sqe, self.fd_, nullptr, nullptr, 0, file_idx );
  io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );

  self.peer_fd_ = file_idx;
  self.h_ = h;
  self.initiated_ = true;
}

acceptor&
acceptor::operator=( acceptor&& rhs ) noexcept {
  if ( this != &rhs ) {
    memcpy( &addr_storage_, &rhs.addr_storage_, sizeof( addr_storage_ ) );

    ex_ = std::move( rhs.ex_ );

    fd_ = rhs.fd_;
    rhs.fd_ = -1;

    is_ipv4_ = rhs.is_ipv4_;
  }

  return *this;
}

acceptor::acceptor( fiona::executor ex, in_addr ipv4_addr, std::uint16_t port )
    : ex_( ex ) {

  memset( &addr_storage_, 0, sizeof( addr_storage_ ) );

  int fd = socket( AF_INET, SOCK_STREAM, 0 );
  if ( fd == -1 ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  int enable = 1;
  if ( -1 ==
       setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof( enable ) ) ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  sockaddr_in addr;
  memset( &addr, 0, sizeof( addr ) );
  addr.sin_family = AF_INET;
  addr.sin_port = htons( port );
  addr.sin_addr.s_addr = ntohl( ipv4_addr.s_addr );

  if ( -1 ==
       bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  constexpr int backlog = 5000;
  if ( -1 == listen( fd, backlog ) ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  fd_ = fd;
  is_ipv4_ = true;
  if ( port == 0 ) {
    socklen_t addrlen = sizeof( sockaddr_in );
    if ( -1 == getsockname( fd_, reinterpret_cast<sockaddr*>( &addr_storage_ ),
                            &addrlen ) ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }
  } else {
    memcpy( &addr_storage_, &addr, sizeof( addr ) );
  }
}

acceptor::acceptor( fiona::executor ex, in6_addr ipv6_addr, std::uint16_t port )
    : ex_( ex ) {
  addr_storage_ = {};

  int fd = socket( AF_INET6, SOCK_STREAM, 0 );
  if ( fd == -1 ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  int enable = 1;
  if ( -1 ==
       setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof( enable ) ) ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  sockaddr_in6 addr = {};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons( port );
  addr.sin6_addr = ipv6_addr;

  if ( -1 ==
       bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  constexpr int backlog = 5000;
  if ( -1 == listen( fd, backlog ) ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  fd_ = fd;
  is_ipv4_ = false;
  if ( port == 0 ) {
    socklen_t addrlen = sizeof( sockaddr_in6 );
    if ( -1 == getsockname( fd_, reinterpret_cast<sockaddr*>( &addr_storage_ ),
                            &addrlen ) ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }
  } else {
    memcpy( &addr_storage_, &addr, sizeof( addr ) );
  }
}

} // namespace tcp
} // namespace fiona
