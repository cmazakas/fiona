#include <cerrno>
#include <fiona/tcp.hpp>

#include <fiona/borrowed_buffer.hpp>         // for borrowed_buffer
#include <fiona/error.hpp>                   // for error_code, result, throw_errno_as_error_code
#include <fiona/executor.hpp>                // for executor_access_policy, executor

#include <fiona/detail/common.hpp>           // for buf_ring
#include <fiona/detail/config.hpp>           // for FIONA_DECL
#include <fiona/detail/get_sqe.hpp>          // for reserve_sqes, submit_ring, get_sqe

#include <boost/assert.hpp>                  // for BOOST_ASSERT
#include <boost/config/detail/suffix.hpp>    // for BOOST_NOINLINE, BOOST_NORETURN
#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <coroutine>                         // for coroutine_handle
#include <cstdint>                           // for uint16_t, uintptr_t
#include <cstring>                           // for memcpy, size_t
#include <deque>                             // for deque
#include <utility>                           // for move

#include <arpa/inet.h>                       // for ntohs
#include <errno.h>                           // for errno, ECANCELED, EBUSY, EINVAL, EISCONN, ETIME, ETIMEDOUT
#include <liburing.h>                        // for io_uring_sqe_set_data, io_uring_get_sqe, io_uring_sqe_se...
#include <liburing/io_uring.h>               // for io_uring_cqe, IOSQE_CQE_SKIP_SUCCESS, IORING_CQE_F_MORE
#include <linux/time_types.h>                // for __kernel_timespec
#include <netinet/in.h>                      // for sockaddr_in, sockaddr_in6, IPPROTO_TCP, in6_addr, in_addr
#include <sys/socket.h>                      // for AF_INET6, AF_INET, sockaddr, sockaddr_storage, bind, get...
#include <unistd.h>                          // for close

#include "awaitable_base.hpp"                // for intrusive_ptr_add_ref, awaitable_base
#include "stream_impl.hpp"

namespace fiona {

namespace tcp {
namespace {
BOOST_NOINLINE BOOST_NORETURN inline void
throw_busy() {
  fiona::detail::throw_errno_as_error_code( EBUSY );
}
} // namespace

namespace detail {

struct acceptor_impl {
  struct accept_frame final : public fiona::detail::awaitable_base {
    acceptor_impl* pacceptor_ = nullptr;
    std::coroutine_handle<> h_;
    int peer_fd_ = -1;
    bool initiated_ = false;
    bool done_ = false;

    accept_frame() = delete;
    accept_frame( acceptor_impl* pacceptor ) : pacceptor_{ pacceptor } {}
    ~accept_frame() = default;

    void reset() {
      h_ = nullptr;
      peer_fd_ = -1;
      initiated_ = false;
      done_ = false;
    }

    void await_process_cqe( io_uring_cqe* cqe ) override {
      auto res = cqe->res;
      if ( res < 0 ) {
        fiona::detail::executor_access_policy::release_fd( pacceptor_->ex_, peer_fd_ );
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

  acceptor_impl( executor ex, sockaddr const* addr, int const backlog ) : ex_{ ex } {
    auto const addrlen = addr->sa_family == AF_INET6 ? sizeof( sockaddr_in6 ) : sizeof( sockaddr_in );

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
    ret = getsockname( fd, reinterpret_cast<sockaddr*>( &addr_storage_ ), &caddrlen );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }
    BOOST_ASSERT( caddrlen == addrlen );
  }

  acceptor_impl( executor ex, sockaddr_in const addr, int const backlog )
      : acceptor_impl( ex, reinterpret_cast<sockaddr const*>( &addr ), backlog ) {}

  acceptor_impl( executor ex, sockaddr_in6 const addr, int const backlog )
      : acceptor_impl( ex, reinterpret_cast<sockaddr const*>( &addr ), backlog ) {}

public:
  acceptor_impl( executor ex, in_addr ipv4_addr, std::uint16_t const port, int const backlog )
      : acceptor_impl( ex,
                       sockaddr_in{ .sin_family = AF_INET, .sin_port = port, .sin_addr = ipv4_addr, .sin_zero = { 0 } },
                       backlog ) {}

  acceptor_impl( executor ex, in6_addr ipv6_addr, std::uint16_t const port, int const backlog )
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

void FIONA_DECL
intrusive_ptr_release( acceptor_impl* pacceptor ) noexcept {
  --pacceptor->count_;
  if ( pacceptor->count_ == 0 ) {
    delete pacceptor;
  }
}

} // namespace detail

inline constexpr int const static default_backlog = 256;

acceptor::acceptor( executor ex, sockaddr const* addr )
    : pacceptor_{ new detail::acceptor_impl( ex, addr, default_backlog ) } {}

std::uint16_t
acceptor::port() const noexcept {
  return pacceptor_->port();
}

executor
acceptor::get_executor() const noexcept {
  return pacceptor_->ex_;
}

accept_awaitable
acceptor::async_accept() {
  return { pacceptor_ };
}

accept_awaitable::accept_awaitable( boost::intrusive_ptr<detail::acceptor_impl> pacceptor ) : pacceptor_{ pacceptor } {}

accept_awaitable::~accept_awaitable() {
  auto& af = pacceptor_->accept_frame_;
  if ( af.initiated_ && !af.done_ ) {
    auto ex = pacceptor_->ex_;
    auto ring = fiona::detail::executor_access_policy::ring( ex );

    fiona::detail::reserve_sqes( ring, 1 );

    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel( sqe, &af, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    fiona::detail::submit_ring( ring );
  }
}

bool
accept_awaitable::await_ready() const {
  return false;
}

void
accept_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto ex = pacceptor_->ex_;
  auto fd = pacceptor_->fd_;
  auto& f = pacceptor_->accept_frame_;
  if ( f.initiated_ ) {
    throw_busy();
  }

  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto file_idx = fiona::detail::executor_access_policy::get_available_fd( ex );
  auto sqe = fiona::detail::get_sqe( ring );

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

  f.reset();
  if ( peer_fd < 0 ) {
    return { error_code::from_errno( -peer_fd ) };
  }

  auto s = stream( ex, peer_fd );
  s.pstream_->connected_ = true;
  return { std::move( s ) };
}

namespace detail {

void FIONA_DECL
intrusive_ptr_add_ref( stream_impl* pstream ) noexcept {
  ++pstream->count_;
}

void FIONA_DECL
intrusive_ptr_release( stream_impl* pstream ) noexcept {
  --pstream->count_;
  if ( pstream->count_ == 0 ) {
    delete pstream;
  }
}
} // namespace detail

stream::stream( executor ex, int fd ) : pstream_{ new detail::stream_impl{ ex, fd } } {}

stream::~stream() {
  cancel_timer();
  cancel_recv();
}

void
stream::timeout( __kernel_timespec ts ) {
  pstream_->ts_ = ts;

  auto ex = pstream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  fiona::detail::reserve_sqes( ring, 2 );

  {
    auto sqe = io_uring_get_sqe( ring );

    io_uring_prep_timeout_remove( sqe, reinterpret_cast<std::uintptr_t>( &pstream_->timeout_frame_ ), 0 );
    io_uring_sqe_set_data( sqe, nullptr /* &pstream_->timeout_frame_ */ );
    io_uring_sqe_set_flags( sqe, /* IOSQE_IO_LINK | */ IOSQE_CQE_SKIP_SUCCESS );
  }
}

void
stream::cancel_timer() {
  if ( pstream_ && pstream_->timeout_frame_.initiated_ ) {
    pstream_->timeout_frame_.cancelled_ = true;
    auto ring = fiona::detail::executor_access_policy::ring( pstream_->ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    {
      auto user_data = reinterpret_cast<std::uintptr_t>( &pstream_->timeout_frame_ );
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout_remove( sqe, user_data, 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      fiona::detail::submit_ring( ring );
    }
  }
}

void
stream::cancel_recv() {
  if ( pstream_ && pstream_->recv_frame_.initiated_ ) {
    auto ring = fiona::detail::executor_access_policy::ring( pstream_->ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_cancel( sqe, &pstream_->recv_frame_, 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      fiona::detail::submit_ring( ring );
    }
  }
}

executor
stream::get_executor() const {
  return pstream_->ex_;
}

void
stream::set_buffer_group( std::uint16_t bgid ) {
  auto& rf = pstream_->recv_frame_;
  if ( rf.initiated_ || rf.num_bufs_ > 0 ) {
    fiona::detail::throw_errno_as_error_code( EBUSY );
  }

  rf.buffer_group_id_ = bgid;
}

stream_close_awaitable
stream::async_close() {
  return { pstream_ };
}

stream_cancel_awaitable
stream::async_cancel() {
  return { pstream_ };
}

send_awaitable
stream::async_send( std::string_view msg ) {
  return async_send( std::span{ reinterpret_cast<unsigned char const*>( msg.data() ), msg.size() } );
}

send_awaitable
stream::async_send( std::span<unsigned char const> buf ) {
  return { buf, pstream_ };
}

recv_awaitable
stream::async_recv() {
  BOOST_ASSERT( pstream_->recv_frame_.buffer_group_id_ >= 0 );
  return { pstream_ };
}

stream_close_awaitable::stream_close_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream )
    : pstream_{ pstream } {}

stream_close_awaitable::~stream_close_awaitable() {}

bool
stream_close_awaitable::await_ready() const {
  if ( pstream_->close_frame_.initiated_ ) {
    throw_busy();
  }
  return false;
}

void
stream_close_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto ex = pstream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  fiona::detail::reserve_sqes( ring, 1 );

  {
    auto fd = pstream_->fd_;
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_close_direct( sqe, fd );
    io_uring_sqe_set_data( sqe, &pstream_->close_frame_ );
  }

  intrusive_ptr_add_ref( pstream_.get() );

  pstream_->close_frame_.initiated_ = true;
  pstream_->close_frame_.h_ = h;
}

result<void>
stream_close_awaitable::await_resume() {
  auto& cf = pstream_->close_frame_;
  auto res = cf.res_;
  cf.reset();

  if ( res == 0 ) {
    return {};
  }

  return { error_code::from_errno( -res ) };
}

stream_cancel_awaitable::stream_cancel_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream )
    : pstream_{ pstream } {}

bool
stream_cancel_awaitable::await_ready() const {
  return pstream_->fd_ == -1;
}

void
stream_cancel_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto ex = pstream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto fd = pstream_->fd_;
  auto& cf = pstream_->cancel_frame_;

  fiona::detail::reserve_sqes( ring, 1 );

  BOOST_ASSERT( fd != -1 );

  auto sqe = io_uring_get_sqe( ring );
  io_uring_prep_cancel_fd( sqe, fd, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED );
  io_uring_sqe_set_data( sqe, boost::intrusive_ptr( &pstream_->cancel_frame_ ).detach() );

  cf.initiated_ = true;
  cf.h_ = h;
  pstream_->cancelled_ = true;
}

result<int>
stream_cancel_awaitable::await_resume() {
  auto fd = pstream_->fd_;
  auto res = pstream_->cancel_frame_.res_;

  pstream_->cancel_frame_.reset();

  if ( fd == -1 ) {
    return { 0 };
  }

  if ( res < 0 ) {
    return { error_code::from_errno( -res ) };
  }

  return { res };
}

send_awaitable::send_awaitable( std::span<unsigned char const> buf, boost::intrusive_ptr<detail::stream_impl> pstream )
    : buf_{ buf }, pstream_{ pstream } {}

send_awaitable::~send_awaitable() {
  if ( pstream_->send_frame_.initiated_ && !pstream_->send_frame_.done_ ) {
    auto ring = fiona::detail::executor_access_policy::ring( pstream_->ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel( sqe, &pstream_->send_frame_, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    fiona::detail::submit_ring( ring );
  }
}

void
send_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& sf = pstream_->send_frame_;

  if ( sf.initiated_ ) {
    throw_busy();
  }

  auto ex = pstream_->ex_;
  auto fd = pstream_->fd_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  fiona::detail::reserve_sqes( ring, 1 );

  {
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_send( sqe, fd, buf_.data(), buf_.size(), 0 );
    io_uring_sqe_set_data( sqe, &sf );
    io_uring_sqe_set_flags( sqe, IOSQE_FIXED_FILE );
  }

  intrusive_ptr_add_ref( &sf );

  sf.initiated_ = true;
  sf.last_send_ = fiona::tcp::detail::stream_impl::clock_type::now();
  sf.h_ = h;
}

result<std::size_t>
send_awaitable::await_resume() {
  auto res = pstream_->send_frame_.res_;

  pstream_->send_frame_.reset();

  if ( res < 0 ) {
    return fiona::error_code::from_errno( -res );
  }

  return { res };
}

recv_awaitable::recv_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream ) : pstream_{ pstream } {}

recv_awaitable::~recv_awaitable() {}

bool
recv_awaitable::await_ready() const {
  return !pstream_->recv_frame_.buffers_.empty();
}

void
recv_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& rf = pstream_->recv_frame_;

  rf.h_ = h;
  if ( rf.initiated_ ) {
    BOOST_ASSERT( rf.buffers_.empty() );
    return;
  }

  rf.pbuf_ring_ =
      fiona::detail::executor_access_policy::get_buffer_group( pstream_->ex_, pstream_->recv_frame_.buffer_group_id_ );
  rf.schedule_recv();
}

result<borrowed_buffer>
recv_awaitable::await_resume() {
  auto buf = std::move( pstream_->recv_frame_.buffers_.front() );
  pstream_->recv_frame_.buffers_.pop_front();
  return buf;
}

client::client( executor ex ) { pstream_ = new detail::client_impl( ex ); }

connect_awaitable
client::async_connect( sockaddr_in6 const* addr ) {
  return async_connect( reinterpret_cast<sockaddr const*>( addr ) );
}
connect_awaitable
client::async_connect( sockaddr_in const* addr ) {
  return async_connect( reinterpret_cast<sockaddr const*>( addr ) );
}

connect_awaitable
client::async_connect( sockaddr const* addr ) {
  auto const is_ipv4 = ( addr->sa_family == AF_INET );

  if ( !is_ipv4 && ( addr->sa_family != AF_INET6 ) ) {
    fiona::detail::throw_errno_as_error_code( EINVAL );
  }

  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  std::memcpy( &pclient->addr_storage_, addr, is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 ) );
  return { pclient };
}

connect_awaitable::connect_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream ) : pstream_{ pstream } {}

connect_awaitable::~connect_awaitable() {
  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  auto ex = pclient->ex_;
  auto& sf = pclient->socket_frame_;
  auto& cf = pclient->connect_frame_;

  auto ring = fiona::detail::executor_access_policy::ring( ex );

  auto reserve_size = static_cast<int>( sf.initiated_ ) + static_cast<int>( cf.initiated_ );
  fiona::detail::reserve_sqes( ring, reserve_size );

  if ( sf.initiated_ && !sf.done_ ) {
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel( sqe, &sf, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
  }

  if ( cf.initiated_ && !cf.done_ ) {
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel( sqe, &cf, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
  }

  fiona::detail::submit_ring( ring );
}

bool
connect_awaitable::await_ready() const {
  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  if ( pclient->socket_frame_.initiated_ ) {
    throw_busy();
  }
  return false;
}

void
connect_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  BOOST_ASSERT( !pclient->socket_frame_.initiated_ );
  BOOST_ASSERT( !pclient->connect_frame_.initiated_ );
  BOOST_ASSERT( !pclient->socket_frame_.h_ );
  BOOST_ASSERT( !pclient->connect_frame_.h_ );

  auto const* addr = &pclient->addr_storage_;
  auto const is_ipv4 = ( addr->ss_family == AF_INET );
  BOOST_ASSERT( is_ipv4 || addr->ss_family == AF_INET6 );

  auto af = is_ipv4 ? AF_INET : AF_INET6;
  auto ex = pclient->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  if ( pclient->fd_ >= 0 && pclient->connected_ ) {
    fiona::detail::reserve_sqes( ring, 3 );
  } else {
    if ( pclient->fd_ == -1 ) {
      auto const file_idx = fiona::detail::executor_access_policy::get_available_fd( ex );

      pclient->fd_ = file_idx;
    }

    fiona::detail::reserve_sqes( ring, 4 );

    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_socket_direct( sqe, af, SOCK_STREAM, IPPROTO_TCP, pclient->fd_, 0 );
      io_uring_sqe_set_data( sqe, &pclient->socket_frame_ );
      io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK );

      intrusive_ptr_add_ref( pstream_.get() );
    }

    pclient->socket_frame_.h_ = h;
    pclient->socket_frame_.initiated_ = true;
  }

  {
    auto addr = reinterpret_cast<sockaddr const*>( &pclient->addr_storage_ );
    auto addrlen = ( is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 ) );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_connect( sqe, pclient->fd_, addr, addrlen );
    io_uring_sqe_set_data( sqe, &pclient->connect_frame_ );
    io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK | IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS );
  }

  {
    auto ts = &pclient->ts_;
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_link_timeout( sqe, ts, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS );
  }

  {
    auto addr = reinterpret_cast<sockaddr const*>( &pclient->addr_storage_ );
    auto addrlen = ( is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 ) );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_connect( sqe, pclient->fd_, addr, addrlen );
    io_uring_sqe_set_data( sqe, &pclient->connect_frame_ );
    io_uring_sqe_set_flags( sqe, IOSQE_FIXED_FILE );
  }

  intrusive_ptr_add_ref( pstream_.get() );
  pclient->connect_frame_.h_ = h;
  pclient->connect_frame_.initiated_ = true;
}

result<void>
connect_awaitable::await_resume() {
  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  auto& socket_frame = pclient->socket_frame_;
  auto& connect_frame = pclient->connect_frame_;

  if ( socket_frame.res_ < 0 ) {
    BOOST_ASSERT( connect_frame.initiated_ );
    BOOST_ASSERT( !connect_frame.done_ );
    auto res = -socket_frame.res_;
    socket_frame.reset();
    connect_frame.reset();
    return { error_code::from_errno( res ) };
  }

  if ( connect_frame.res_ < 0 ) {
    BOOST_ASSERT( ( !socket_frame.initiated_ && !socket_frame.done_ ) ||
                  ( socket_frame.initiated_ && socket_frame.done_ ) );

    auto res = -connect_frame.res_;
    socket_frame.reset();
    connect_frame.reset();
    return { error_code::from_errno( res ) };
  }
  socket_frame.reset();
  connect_frame.reset();
  return {};
}

} // namespace tcp
} // namespace fiona
