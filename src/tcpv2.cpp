#include <fiona/io_context.hpp>
#include <fiona/tcpv2.hpp>

#include <fiona/detail/awaitable_base.hpp>
#include <fiona/detail/get_sqe.hpp>

#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace fiona {

namespace {
BOOST_NOINLINE BOOST_NORETURN inline void
throw_busy() {
  detail::throw_errno_as_error_code( EBUSY );
}
} // namespace

namespace detail {

struct acceptor_impl {
private:
  friend struct fiona::accept_awaitable;

  struct accept_frame final : public awaitable_base {
    acceptor_impl* pacceptor_ = nullptr;
    std::coroutine_handle<> h_;
    int peer_fd_ = -1;
    bool initiated_ = false;
    bool done_ = false;

    accept_frame() = delete;
    accept_frame( acceptor_impl* pacceptor ) : pacceptor_{ pacceptor } {}
    ~accept_frame() = default;

    void await_process_cqe( io_uring_cqe* cqe ) override {
      auto res = cqe->res;
      if ( res < 0 ) {
        executor_access_policy::release_fd( pacceptor_->ex_, peer_fd_ );
        peer_fd_ = res;
      }
      done_ = true;
    }

    std::coroutine_handle<> handle() noexcept override {
      auto h = h_;
      h_ = nullptr;
      return h;
    }

    void inc_ref() noexcept override { ++pacceptor_->count_; }
    void dec_ref() noexcept override {
      --pacceptor_->count_;
      if ( pacceptor_->count_ == 0 ) {
        delete pacceptor_;
      }
    }

    int use_count() const noexcept override { return pacceptor_->count_; }
  };

  sockaddr_storage addr_storage_ = {};
  accept_frame accept_frame_{ this };
  executor ex_;
  int fd_ = -1;
  int count_ = 0;
  bool is_ipv4_ = true;

  friend void intrusive_ptr_add_ref( acceptor_impl* pacceptor ) noexcept;
  friend void intrusive_ptr_release( acceptor_impl* pacceptor ) noexcept;

  acceptor_impl( executor ex, sockaddr const* addr, socklen_t const addrlen,
                 int const backlog )
      : ex_{ ex } {
    auto const is_ipv4 = ( addr->sa_family == AF_INET );
    BOOST_ASSERT( is_ipv4 || addr->sa_family == AF_INET6 );

    auto af = is_ipv4 ? AF_INET : AF_INET6;

    int ret = -1;
    int fd = socket( af, SOCK_STREAM, 0 );
    if ( fd == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    int const enable = 1;
    ret = setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof( enable ) );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    ret = bind( fd, addr, addrlen );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    ret = listen( fd, backlog );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    fd_ = fd;
    is_ipv4_ = is_ipv4;

    socklen_t caddrlen = sizeof( addr_storage_ );
    ret = getsockname( fd, reinterpret_cast<sockaddr*>( &addr_storage_ ),
                       &caddrlen );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }
    BOOST_ASSERT( caddrlen == addrlen );
  }

  acceptor_impl( executor ex, sockaddr_in const addr, int const backlog )
      : acceptor_impl( ex, reinterpret_cast<sockaddr const*>( &addr ),
                       sizeof( addr ), backlog ) {}

  acceptor_impl( executor ex, sockaddr_in6 const addr, int const backlog )
      : acceptor_impl( ex, reinterpret_cast<sockaddr const*>( &addr ),
                       sizeof( addr ), backlog ) {}

public:
  acceptor_impl( executor ex, in_addr ipv4_addr, std::uint16_t const port,
                 int const backlog )
      : acceptor_impl( ex,
                       sockaddr_in{ .sin_family = AF_INET,
                                    .sin_port = port,
                                    .sin_addr = ipv4_addr,
                                    .sin_zero = { 0 } },
                       backlog ) {}

  acceptor_impl( executor ex, in6_addr ipv6_addr, std::uint16_t const port,
                 int const backlog )
      : acceptor_impl( ex,
                       sockaddr_in6{ .sin6_family = AF_INET6,
                                     .sin6_port = port,
                                     .sin6_flowinfo = 0,
                                     .sin6_addr = ipv6_addr,
                                     .sin6_scope_id = 0 },
                       backlog ) {}

  ~acceptor_impl() {
    if ( fd_ >= 0 ) {
      close( fd_ );
    }
  }

  std::uint16_t port() const noexcept {
    if ( is_ipv4_ ) {
      auto paddr = reinterpret_cast<sockaddr_in const*>( &addr_storage_ );
      return ntohs( paddr->sin_port );
    }

    auto paddr = reinterpret_cast<sockaddr_in6 const*>( &addr_storage_ );
    return ntohs( paddr->sin6_port );
  }
};

void
intrusive_ptr_add_ref( acceptor_impl* pacceptor ) noexcept {
  ++pacceptor->count_;
}

void
intrusive_ptr_release( acceptor_impl* pacceptor ) noexcept {
  --pacceptor->count_;
  if ( pacceptor->count_ == 0 ) {
    delete pacceptor;
  }
}

struct stream_impl {
private:
  friend struct fiona::stream;

  executor ex_;
  int count_ = 0;
  int fd_ = -1;

  stream_impl( executor ex, int fd ) : ex_{ ex }, fd_{ fd } {}

  friend void intrusive_ptr_add_ref( stream_impl* pstream ) noexcept;
  friend void intrusive_ptr_release( stream_impl* pstream ) noexcept;

public:
  ~stream_impl() {
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
};

void
intrusive_ptr_add_ref( stream_impl* pstream ) noexcept {
  ++pstream->count_;
}

void
intrusive_ptr_release( stream_impl* pstream ) noexcept {
  --pstream->count_;
  if ( pstream->count_ == 0 ) {
    delete pstream;
  }
}

struct client_impl : public stream_impl {};

} // namespace detail

inline constexpr int const static default_backlog = 256;

acceptor::acceptor( executor ex, in_addr ipv4_addr, std::uint16_t const port )
    : acceptor( ex, ipv4_addr, port, default_backlog ) {}

acceptor::acceptor( executor ex, in_addr ipv4_addr, std::uint16_t const port,
                    int const backlog )
    : pacceptor_{ new detail::acceptor_impl( ex, ipv4_addr, port, backlog ) } {}

acceptor::acceptor( executor ex, in6_addr ipv6_addr, std::uint16_t const port )
    : acceptor( ex, ipv6_addr, port, default_backlog ) {}

acceptor::acceptor( executor ex, in6_addr ipv6_addr, std::uint16_t const port,
                    int const backlog )
    : pacceptor_{ new detail::acceptor_impl( ex, ipv6_addr, port, backlog ) } {}

std::uint16_t
acceptor::port() const noexcept {
  return pacceptor_->port();
}

accept_awaitable
acceptor::async_accept() {
  return { pacceptor_ };
}

stream::stream( executor ex, int fd )
    : pstream_{ new detail::stream_impl{ ex, fd } } {}

accept_awaitable::accept_awaitable(
    boost::intrusive_ptr<detail::acceptor_impl> pacceptor )
    : pacceptor_{ pacceptor } {}

bool
accept_awaitable::await_ready() const {
  return false;
}

void
accept_awaitable::await_suspend( std::coroutine_handle<> h ) noexcept {
  auto ex = pacceptor_->ex_;
  auto fd = pacceptor_->fd_;
  auto& f = pacceptor_->accept_frame_;
  if ( f.initiated_ ) {
    throw_busy();
  }

  auto ring = detail::executor_access_policy::ring( ex );
  auto file_idx = detail::executor_access_policy::get_available_fd( ex );
  auto sqe = detail::get_sqe( ring );

  io_uring_prep_accept_direct( sqe, fd, nullptr, nullptr, 0, file_idx );
  io_uring_sqe_set_data( sqe, boost::intrusive_ptr( &f ).detach() );

  f.peer_fd_ = file_idx;
  f.initiated_ = true;
  f.h_ = h;
}

result<stream>
accept_awaitable::await_resume() {
  auto ex = pacceptor_->ex_;
  auto& f = pacceptor_->accept_frame_;
  auto peer_fd = f.peer_fd_;
  if ( peer_fd < 0 ) {
    return { error_code::from_errno( -peer_fd ) };
  }
  return { stream( ex, peer_fd ) };
}

} // namespace fiona
