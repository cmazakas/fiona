// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <fiona/tcp.hpp>                     // for stream, send_awa...

#include <fiona/buffer.hpp>                  // for recv_buffer
#include <fiona/detail/common.hpp>
#include <fiona/detail/get_sqe.hpp>          // for reserve_sqes
#include <fiona/error.hpp>                   // for error_code, result
#include <fiona/executor.hpp>                // for executor_access_...

#include <boost/assert.hpp>                  // for BOOST_ASSERT
#include <boost/config/detail/suffix.hpp>    // for BOOST_NOINLINE
#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <coroutine>                         // for coroutine_handle
#include <cstdint>                           // for uint16_t, uintptr_t
#include <cstring>                           // for memcpy, size_t
#include <deque>                             // for deque
#include <span>                              // for span
#include <string_view>                       // for string_view
#include <utility>                           // for move

#include <arpa/inet.h>                       // for ntohs
#include <errno.h>                           // for errno, EBUSY
#include <liburing.h>                        // for io_uring_sqe_set...
#include <liburing/io_uring.h>               // for IOSQE_CQE_SKIP_S...
#include <linux/time_types.h>                // for __kernel_timespec
#include <netinet/in.h>                      // for sockaddr_in, soc...
#include <sys/socket.h>                      // for AF_INET6, AF_INET
#include <unistd.h>                          // for close

#include "detail/awaitable_base.hpp"         // for intrusive_ptr_ad...
#include "detail/stream_impl.hpp"            // for stream_impl, cli...

namespace fiona {

namespace tcp {
namespace {

BOOST_NOINLINE BOOST_NORETURN inline void
throw_busy()
{
  fiona::detail::throw_errno_as_error_code( EBUSY );
}
} // namespace

namespace detail {

stream_impl::~stream_impl()
{
  if ( fd_ >= 0 ) {
    auto ring = fiona::detail::executor_access_policy::ring( ex_ );
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_close_direct( sqe, static_cast<unsigned>( fd_ ) );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    fiona::detail::submit_ring( ring );

    fiona::detail::executor_access_policy::release_fd( ex_, fd_ );
    fd_ = -1;
  }
}

stream_impl::cancel_frame::~cancel_frame() = default;
stream_impl::timeout_cancel_frame::~timeout_cancel_frame() = default;
stream_impl::close_frame::~close_frame() = default;
stream_impl::shutdown_frame::~shutdown_frame() = default;
stream_impl::send_frame::~send_frame() = default;
stream_impl::recv_frame::~recv_frame() = default;
stream_impl::timeout_frame::~timeout_frame() = default;

//------------------------------------------------------------------------------

struct accept_frame : public fiona::detail::awaitable_base
{
  acceptor_impl* p_acceptor_ = nullptr;
  std::coroutine_handle<> h_;
  int peer_fd_ = -1;
  bool initiated_ = false;
  bool done_ = false;

  accept_frame() = delete;
  accept_frame( acceptor_impl* p_acceptor ) : p_acceptor_( p_acceptor ) {}
  virtual ~accept_frame() override;

  void
  reset()
  {
    h_ = nullptr;
    peer_fd_ = -1;
    initiated_ = false;
    done_ = false;
  }

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }
};

//------------------------------------------------------------------------------

struct acceptor_impl : public virtual ref_count, public accept_frame
{
  sockaddr_storage addr_storage_ = {};
  executor ex_;
  int fd_ = -1;
  bool is_ipv4_ = true;

  acceptor_impl( executor ex, sockaddr const* addr, int const backlog )
      : accept_frame( this ), ex_( ex )
  {
    auto const addrlen = static_cast<socklen_t>( addr->sa_family == AF_INET6
                                                     ? sizeof( sockaddr_in6 )
                                                     : sizeof( sockaddr_in ) );

    auto const is_ipv4 = ( addr->sa_family == AF_INET );
    BOOST_ASSERT( is_ipv4 || addr->sa_family == AF_INET6 );

    auto af = is_ipv4 ? AF_INET : AF_INET6;

    int ret = -1;
    int fd = socket( af, SOCK_STREAM, 0 );
    if ( fd == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    // TODO: now that we're writing tests correctly, we don't seem to need this
    // hard-coded but a user might find it useful later on as something they can
    // set optionally

    // int const enable = 1;
    // ret = setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof( enable )
    // ); if ( ret == -1 ) {
    //   fiona::detail::throw_errno_as_error_code( errno );
    // }

    ret = bind( fd, addr, addrlen );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    ret = listen( fd, backlog );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    socklen_t caddrlen = sizeof( addr_storage_ );
    ret = getsockname( fd, reinterpret_cast<sockaddr*>( &addr_storage_ ),
                       &caddrlen );
    if ( ret == -1 ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }
    BOOST_ASSERT( caddrlen == addrlen );

    auto off = fiona::detail::executor_access_policy::get_available_fd( ex_ );
    if ( off < 0 ) {
      throw error_code::from_errno( ENFILE );
    }

    auto ring = fiona::detail::executor_access_policy::ring( ex_ );
    ret = io_uring_register_files_update( ring, static_cast<unsigned>( off ),
                                          &fd, 1 );

    if ( ret < 0 ) {
      fiona::detail::throw_errno_as_error_code( -ret );
    }

    // fd_ = fd;
    fd_ = off;
    is_ipv4_ = is_ipv4;
    close( fd );
  }

  acceptor_impl( executor ex, sockaddr_in const addr, int const backlog )
      : acceptor_impl( ex, reinterpret_cast<sockaddr const*>( &addr ), backlog )
  {
  }

  acceptor_impl( executor ex, sockaddr_in6 const addr, int const backlog )
      : acceptor_impl( ex, reinterpret_cast<sockaddr const*>( &addr ), backlog )
  {
  }

public:
  acceptor_impl( executor ex,
                 in_addr ipv4_addr,
                 std::uint16_t const port,
                 int const backlog )
      : acceptor_impl( ex,
                       sockaddr_in{ .sin_family = AF_INET,
                                    .sin_port = port,
                                    .sin_addr = ipv4_addr,
                                    .sin_zero = { 0 } },
                       backlog )
  {
  }

  acceptor_impl( executor ex,
                 in6_addr ipv6_addr,
                 std::uint16_t const port,
                 int const backlog )
      : acceptor_impl( ex,
                       sockaddr_in6{ .sin6_family = AF_INET6,
                                     .sin6_port = port,
                                     .sin6_flowinfo = 0,
                                     .sin6_addr = ipv6_addr,
                                     .sin6_scope_id = 0 },
                       backlog )
  {
  }

  virtual ~acceptor_impl() override;

  std::uint16_t
  port() const noexcept
  {
    if ( is_ipv4_ ) {
      auto paddr = reinterpret_cast<sockaddr_in const*>( &addr_storage_ );
      return ntohs( paddr->sin_port );
    }

    auto paddr = reinterpret_cast<sockaddr_in6 const*>( &addr_storage_ );
    return ntohs( paddr->sin6_port );
  }
};

void FIONA_EXPORT
intrusive_ptr_add_ref( acceptor_impl* p_acceptor ) noexcept
{
  intrusive_ptr_add_ref( static_cast<ref_count*>( p_acceptor ) );
}

void FIONA_EXPORT
intrusive_ptr_release( acceptor_impl* p_acceptor ) noexcept
{
  intrusive_ptr_release( static_cast<ref_count*>( p_acceptor ) );
}

acceptor_impl::~acceptor_impl()
{
  if ( fd_ >= 0 ) {
    auto ring = fiona::detail::executor_access_policy::ring( ex_ );
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_close_direct( sqe, static_cast<unsigned>( fd_ ) );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    fiona::detail::submit_ring( ring );

    fiona::detail::executor_access_policy::release_fd( ex_, fd_ );
    fd_ = -1;
  }
}

//------------------------------------------------------------------------------

void
accept_frame::await_process_cqe( io_uring_cqe* cqe )
{
  auto res = cqe->res;
  if ( res < 0 ) {
    BOOST_ASSERT( peer_fd_ >= 0 );
    fiona::detail::executor_access_policy::release_fd( p_acceptor_->ex_,
                                                       peer_fd_ );
    peer_fd_ = res;
  }
  done_ = true;
}

accept_frame::~accept_frame() {}

} // namespace detail

inline constexpr int static const default_backlog = 256;

acceptor::acceptor( executor ex, sockaddr const* addr )
    : p_acceptor_{ new detail::acceptor_impl( ex, addr, default_backlog ) }
{
}

std::uint16_t
acceptor::port() const noexcept
{
  return p_acceptor_->port();
}

executor
acceptor::get_executor() const noexcept
{
  return p_acceptor_->ex_;
}

accept_awaitable
acceptor::async_accept()
{
  return { p_acceptor_ };
}

accept_raw_awaitable
acceptor::async_accept_raw()
{
  return { p_acceptor_ };
}

//------------------------------------------------------------------------------

accept_awaitable::accept_awaitable(
    boost::intrusive_ptr<detail::acceptor_impl> pacceptor )
    : p_acceptor_( pacceptor )
{
}

accept_awaitable::~accept_awaitable()
{
  auto& af = static_cast<detail::accept_frame&>( *p_acceptor_ );
  if ( af.initiated_ && !af.done_ ) {
    auto ex = p_acceptor_->ex_;
    auto ring = fiona::detail::executor_access_policy::ring( ex );

    fiona::detail::reserve_sqes( ring, 1 );

    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel( sqe, &af, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    fiona::detail::submit_ring( ring );
  }
}

void
accept_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto ex = p_acceptor_->ex_;
  auto fd = p_acceptor_->fd_;
  auto& af = static_cast<detail::accept_frame&>( *p_acceptor_ );
  if ( af.initiated_ ) {
    throw_busy();
  }

  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto file_idx = fiona::detail::executor_access_policy::get_available_fd( ex );
  if ( file_idx < 0 ) {
    fiona::detail::throw_errno_as_error_code( ENFILE );
  }

  fiona::detail::reserve_sqes( ring, 1 );

  auto sqe = io_uring_get_sqe( ring );
  io_uring_prep_accept_direct( sqe, fd, nullptr, nullptr, 0,
                               static_cast<unsigned>( file_idx ) );
  io_uring_sqe_set_data( sqe, &af );
  io_uring_sqe_set_flags( sqe, IOSQE_FIXED_FILE );

  intrusive_ptr_add_ref( &af );

  af.initiated_ = true;
  af.h_ = h;
  af.peer_fd_ = file_idx;
}

result<stream>
accept_awaitable::await_resume()
{
  auto ex = p_acceptor_->ex_;
  auto& af = static_cast<detail::accept_frame&>( *p_acceptor_ );
  auto peer_fd = af.peer_fd_;

  af.reset();
  if ( peer_fd < 0 ) {
    return { error_code::from_errno( -peer_fd ) };
  }

  auto s = stream( ex, peer_fd );
  s.p_stream_->connected_ = true;
  return { std::move( s ) };
}

//------------------------------------------------------------------------------

accept_raw_awaitable::accept_raw_awaitable(
    boost::intrusive_ptr<detail::acceptor_impl> pacceptor )
    : accept_awaitable( pacceptor )
{
}

result<int>
accept_raw_awaitable::await_resume()
{
  auto ex = p_acceptor_->ex_;
  auto& af = static_cast<detail::accept_frame&>( *p_acceptor_ );
  auto peer_fd = af.peer_fd_;

  af.reset();
  if ( peer_fd < 0 ) {
    return { error_code::from_errno( -peer_fd ) };
  }

  return peer_fd;
}

//------------------------------------------------------------------------------

namespace detail {

void FIONA_EXPORT
intrusive_ptr_add_ref( stream_impl* p_stream ) noexcept
{
  intrusive_ptr_add_ref( static_cast<ref_count*>( p_stream ) );
}

void FIONA_EXPORT
intrusive_ptr_release( stream_impl* p_stream ) noexcept
{
  intrusive_ptr_release( static_cast<ref_count*>( p_stream ) );
}
} // namespace detail

//------------------------------------------------------------------------------

stream::stream( executor ex, int fd )
    : p_stream_( new detail::stream_impl( ex, fd ) )
{
}

stream::~stream()
{
  if ( !p_stream_ ) {
    return;
  }

  auto use_count = p_stream_->use_count() -
                   p_stream_->detail::cancel_frame::is_active() -
                   p_stream_->detail::close_frame::is_active() -
                   p_stream_->detail::shutdown_frame::is_active() -
                   p_stream_->detail::send_frame::is_active() -
                   p_stream_->detail::recv_frame::is_active() -
                   p_stream_->detail::timeout_frame::is_active() -
                   p_stream_->detail::timeout_cancel_frame::is_active();

  BOOST_ASSERT( use_count > 0 );

  if ( use_count == 1 ) {
    cancel_timer();
    cancel_recv();
    p_stream_ = nullptr;
  }
}

void
stream::timeout( __kernel_timespec ts )
{
  p_stream_->ts_ = ts;

  auto ex = p_stream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  fiona::detail::reserve_sqes( ring, 1 );

  {
    auto sqe = io_uring_get_sqe( ring );

    io_uring_prep_timeout_remove(
        sqe,
        reinterpret_cast<std::uintptr_t>(
            static_cast<detail::timeout_frame*>( p_stream_.get() ) ),
        0 );
    io_uring_sqe_set_data( sqe, nullptr /* &pstream_->timeout_frame_ */ );
    io_uring_sqe_set_flags( sqe, /* IOSQE_IO_LINK | */ IOSQE_CQE_SKIP_SUCCESS );
  }
}

void
stream::cancel_timer()
{
  if ( p_stream_ && p_stream_->timeout_frame::initiated_ ) {
    p_stream_->timeout_frame::cancelled_ = true;
    auto ring = fiona::detail::executor_access_policy::ring( p_stream_->ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    {
      auto user_data = reinterpret_cast<std::uintptr_t>(
          static_cast<detail::timeout_frame*>( p_stream_.get() ) );
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout_remove( sqe, user_data, 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      fiona::detail::submit_ring( ring );
    }
  }
}

void
stream::cancel_recv()
{
  if ( p_stream_ && p_stream_->recv_frame::initiated_ ) {
    auto ring = fiona::detail::executor_access_policy::ring( p_stream_->ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_cancel(
          sqe, static_cast<detail::recv_frame*>( p_stream_.get() ), 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      fiona::detail::submit_ring( ring );
    }
  }
}

executor
stream::get_executor() const noexcept
{
  return p_stream_->ex_;
}

void
stream::set_buffer_group( std::uint16_t bgid )
{
  auto& rf = static_cast<detail::recv_frame&>( *p_stream_ );
  if ( rf.initiated_ ) {
    throw_busy();
  }

  auto ex = p_stream_->ex_;
  auto p_br =
      fiona::detail::executor_access_policy::get_buffer_group( ex, bgid );

  if ( !p_br ) {
    fiona::detail::throw_errno_as_error_code( EINVAL );
  }

  rf.buffer_group_id_ = bgid;
}

stream_close_awaitable
stream::async_close()
{
  return { p_stream_ };
}

stream_cancel_awaitable
stream::async_cancel()
{
  return { p_stream_ };
}

send_awaitable
stream::async_send( std::string_view msg )
{
  return async_send( std::span{
      reinterpret_cast<unsigned char const*>( msg.data() ), msg.size() } );
}

send_awaitable
stream::async_send( std::span<unsigned char const> buf )
{
  return { buf, p_stream_ };
}

recv_awaitable
stream::async_recv()
{
  // BOOST_ASSERT( pstream_->recv_frame::buffer_group_id_ >= 0 );
  return { p_stream_ };
}

shutdown_awaitable
stream::async_shutdown( int how )
{
  p_stream_->shutdown_frame::how_ = how;
  return { p_stream_ };
}

//------------------------------------------------------------------------------

stream_close_awaitable::stream_close_awaitable(
    boost::intrusive_ptr<detail::stream_impl> pstream )
    : p_stream_( pstream )
{
}

stream_close_awaitable::~stream_close_awaitable() {}

void
stream_close_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto ex = p_stream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  fiona::detail::reserve_sqes( ring, 1 );

  {
    auto fd = p_stream_->fd_;
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_close_direct( sqe, static_cast<unsigned>( fd ) );
    io_uring_sqe_set_data(
        sqe, static_cast<detail::close_frame*>( p_stream_.get() ) );
  }

  intrusive_ptr_add_ref( p_stream_.get() );

  p_stream_->close_frame::initiated_ = true;
  p_stream_->close_frame::h_ = h;
}

result<void>
stream_close_awaitable::await_resume()
{
  auto& cf = static_cast<detail::close_frame&>( *p_stream_ );
  auto res = cf.res_;
  cf.reset();

  if ( res == 0 ) {
    return {};
  }

  return { error_code::from_errno( -res ) };
}

//------------------------------------------------------------------------------

stream_cancel_awaitable::stream_cancel_awaitable(
    boost::intrusive_ptr<detail::stream_impl> pstream )
    : p_stream_( pstream )
{
}

void
stream_cancel_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto ex = p_stream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto fd = p_stream_->fd_;
  auto& cf = static_cast<detail::cancel_frame&>( *p_stream_ );

  fiona::detail::reserve_sqes( ring, 1 );

  BOOST_ASSERT( fd != -1 );

  auto sqe = io_uring_get_sqe( ring );
  io_uring_prep_cancel_fd(
      sqe, fd, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED );
  io_uring_sqe_set_data(
      sqe, static_cast<detail::cancel_frame*>( p_stream_.get() ) );

  intrusive_ptr_add_ref( &cf );

  cf.initiated_ = true;
  cf.h_ = h;
  p_stream_->stream_cancelled_ = true;
}

result<int>
stream_cancel_awaitable::await_resume()
{
  auto fd = p_stream_->fd_;
  auto res = p_stream_->cancel_frame::res_;

  p_stream_->cancel_frame::reset();

  if ( fd == -1 ) {
    return { 0 };
  }

  if ( res < 0 ) {
    return { error_code::from_errno( -res ) };
  }

  return { res };
}

//------------------------------------------------------------------------------

shutdown_awaitable::shutdown_awaitable(
    boost::intrusive_ptr<detail::stream_impl> p_stream )
    : p_stream_( p_stream )
{
}

shutdown_awaitable::~shutdown_awaitable() = default;

void
shutdown_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto ex = p_stream_->ex_;
  auto fd = p_stream_->fd_;
  BOOST_ASSERT( fd != -1 );

  auto& sf = static_cast<detail::shutdown_frame&>( *p_stream_ );
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  fiona::detail::reserve_sqes( ring, 1 );

  auto sqe = io_uring_get_sqe( ring );
  io_uring_prep_shutdown( sqe, fd, sf.how_ );
  io_uring_sqe_set_data(
      sqe, static_cast<detail::shutdown_frame*>( p_stream_.get() ) );
  io_uring_sqe_set_flags( sqe, IOSQE_FIXED_FILE );

  intrusive_ptr_add_ref( &sf );

  sf.initiated_ = true;
  sf.h_ = h;
}

result<void>
shutdown_awaitable::await_resume()
{
  auto& sf = static_cast<detail::shutdown_frame&>( *p_stream_ );
  auto res = sf.res_;
  sf.reset();

  if ( res == 0 ) {
    return {};
  }

  return { error_code::from_errno( -res ) };
}

//------------------------------------------------------------------------------

send_awaitable::send_awaitable(
    std::span<unsigned char const> buf,
    boost::intrusive_ptr<detail::stream_impl> p_stream )
    : buf_( buf ), p_stream_( p_stream )
{
}

send_awaitable::~send_awaitable()
{
  if ( p_stream_->send_frame::initiated_ && !p_stream_->send_frame::done_ ) {
    auto ring = fiona::detail::executor_access_policy::ring( p_stream_->ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel(
        sqe, static_cast<detail::send_frame*>( p_stream_.get() ), 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    fiona::detail::submit_ring( ring );
  }
}

void
send_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto& sf = static_cast<detail::send_frame&>( *p_stream_ );

  if ( sf.initiated_ ) {
    throw_busy();
  }

  auto ex = p_stream_->ex_;
  auto fd = p_stream_->fd_;
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
  sf.last_send_ = fiona::tcp::detail::clock_type::now();
  sf.h_ = h;
}

result<std::size_t>
send_awaitable::await_resume()
{
  auto res = p_stream_->send_frame::res_;

  p_stream_->send_frame::reset();

  if ( res < 0 ) {
    return fiona::error_code::from_errno( -res );
  }

  return { res };
}

//------------------------------------------------------------------------------

recv_awaitable::recv_awaitable(
    boost::intrusive_ptr<detail::stream_impl> pstream )
    : p_stream_( pstream )
{
}

recv_awaitable::~recv_awaitable() {}

bool
recv_awaitable::await_ready() const
{
  return !p_stream_->recv_frame::buffers_.empty();
}

bool
recv_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto& rf = static_cast<detail::recv_frame&>( *p_stream_ );
  rf.h_ = h;

  if ( rf.ec_ ) {
    BOOST_ASSERT( rf.buffers_.empty() );
    return false;
  }

  if ( rf.initiated_ ) {
    BOOST_ASSERT( rf.buffers_.empty() );
    return true;
  }

  auto bgid = p_stream_->recv_frame::buffer_group_id_;
  if ( bgid == -1 ) {
    fiona::detail::throw_errno_as_error_code( EINVAL );
  }

  rf.pbuf_ring_ = fiona::detail::executor_access_policy::get_buffer_group(
      p_stream_->ex_, static_cast<unsigned>( bgid ) );
  rf.schedule_recv();
  return true;
}

result<recv_buffer_sequence>
recv_awaitable::await_resume()
{
  auto& rf = static_cast<detail::recv_frame&>( *p_stream_ );

  BOOST_ASSERT( !rf.buffers_.empty() || rf.ec_ );

  if ( rf.buffers_.empty() ) {
    auto ec = std::move( rf.ec_ );
    rf.ec_.clear();
    return ec;
  }

  auto pbuf_ring = fiona::detail::executor_access_policy::get_buffer_group(
      p_stream_->ex_,
      static_cast<std::size_t>( p_stream_->recv_frame::buffer_group_id_ ) );

  int len = 0;
  for ( auto buffer : rf.buffers_ ) {
    if ( buffer.empty() ) {
      break;
    }

    if ( pbuf_ring->buf_id_pos_ == pbuf_ring->buf_ids_.data() ) {
      break;
    }

    BOOST_ASSERT( pbuf_ring->buf_id_pos_ != pbuf_ring->buf_ids_.data() );
    BOOST_ASSERT( pbuf_ring->buf_size_ > 0 );

    auto buffer_id = *( --pbuf_ring->buf_id_pos_ );
    auto& buf = pbuf_ring->get_buf( buffer_id );
    BOOST_ASSERT( buf.capacity() == 0 );

    buf = fiona::recv_buffer( pbuf_ring->buf_size_ );
    BOOST_ASSERT( buf.capacity() > 0 );
    io_uring_buf_ring_add( pbuf_ring->as_ptr(), buf.data(),
                           static_cast<unsigned>( buf.capacity() ),
                           static_cast<unsigned short>( buffer_id ),
                           io_uring_buf_ring_mask( pbuf_ring->size() ), len );
    ++len;
  }

  if ( len > 0 ) {
    io_uring_buf_ring_advance( pbuf_ring->as_ptr(), len );
  }

  auto buffers = std::move( rf.buffers_ );
  BOOST_ASSERT( rf.buffers_.empty() );
  BOOST_ASSERT( buffers.num_bufs() > 0 );
  return buffers;
}

namespace detail {

client_impl::~client_impl() {}
client_impl::socket_frame::~socket_frame() {}
client_impl::connect_frame::~connect_frame() {}

} // namespace detail

client::client( executor ex ) { p_stream_ = new detail::client_impl( ex ); }
client::~client()
{
  if ( !p_stream_ ) {
    return;
  }

  auto p_client = static_cast<detail::client_impl*>( p_stream_.get() );

  auto use_count = p_client->use_count() -
                   p_client->detail::cancel_frame::is_active() -
                   p_client->detail::close_frame::is_active() -
                   p_client->detail::shutdown_frame::is_active() -
                   p_client->detail::send_frame::is_active() -
                   p_client->detail::recv_frame::is_active() -
                   p_client->detail::timeout_frame::is_active() -
                   p_client->detail::timeout_cancel_frame::is_active() -
                   p_client->detail::socket_frame::is_active() -
                   p_client->detail::connect_frame::is_active();

  BOOST_ASSERT( use_count > 0 );
  if ( use_count == 1 ) {
    cancel_timer();
    cancel_recv();
    p_stream_ = nullptr;
  }
}

connect_awaitable
client::async_connect( sockaddr_in6 const* addr )
{
  return async_connect( reinterpret_cast<sockaddr const*>( addr ) );
}
connect_awaitable
client::async_connect( sockaddr_in const* addr )
{
  return async_connect( reinterpret_cast<sockaddr const*>( addr ) );
}

connect_awaitable
client::async_connect( sockaddr const* addr )
{
  auto const is_ipv4 = ( addr->sa_family == AF_INET );

  if ( !is_ipv4 && ( addr->sa_family != AF_INET6 ) ) {
    fiona::detail::throw_errno_as_error_code( EINVAL );
  }

  auto pclient = static_cast<detail::client_impl*>( p_stream_.get() );

  std::memcpy( &pclient->addr_storage_, addr,
               is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 ) );
  return { pclient };
}

connect_awaitable::connect_awaitable(
    boost::intrusive_ptr<detail::stream_impl> pstream )
    : pstream_( pstream )
{
}

connect_awaitable::~connect_awaitable()
{
  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  auto ex = pclient->ex_;
  auto& sf = static_cast<detail::socket_frame&>( *pclient );
  auto& cf = static_cast<detail::connect_frame&>( *pclient );

  auto ring = fiona::detail::executor_access_policy::ring( ex );

  // TODO: in general, this seems a bit wrong now that I look at it a second
  // time... links should auto-sever once an earlier entry is cancelled
  // this needs more tests
  unsigned reserve_size = sf.initiated_ + cf.initiated_;
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
connect_awaitable::await_ready() const
{
  auto p_client = static_cast<detail::client_impl*>( pstream_.get() );
  if ( p_client->socket_frame::initiated_ ) {
    throw_busy();
  }
  return false;
}

bool
connect_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  BOOST_ASSERT( !pclient->socket_frame::initiated_ );
  BOOST_ASSERT( !pclient->connect_frame::initiated_ );
  BOOST_ASSERT( !pclient->socket_frame::h_ );
  BOOST_ASSERT( !pclient->connect_frame::h_ );

  auto const* addr = &pclient->addr_storage_;
  auto const is_ipv4 = ( addr->ss_family == AF_INET );
  BOOST_ASSERT( is_ipv4 || addr->ss_family == AF_INET6 );

  auto af = is_ipv4 ? AF_INET : AF_INET6;
  auto ex = pclient->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  if ( pclient->fd_ >= 0 && pclient->connected_ ) {
    fiona::detail::reserve_sqes( ring, 2 );
  } else {
    if ( pclient->fd_ == -1 ) {
      pclient->fd_ =
          fiona::detail::executor_access_policy::get_available_fd( ex );

      if ( pclient->fd_ < 0 ) {
        pclient->socket_frame::res_ = -ENFILE;
        return false;
      }
    }

    fiona::detail::reserve_sqes( ring, 3 );

    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_socket_direct( sqe, af, SOCK_STREAM, IPPROTO_TCP,
                                   static_cast<unsigned>( pclient->fd_ ), 0 );
      io_uring_sqe_set_data( sqe,
                             static_cast<detail::socket_frame*>( pclient ) );
      io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK );

      intrusive_ptr_add_ref( pstream_.get() );
    }

    pclient->socket_frame::h_ = h;
    pclient->socket_frame::initiated_ = true;
  }

  {
    auto addr = reinterpret_cast<sockaddr const*>( &pclient->addr_storage_ );
    socklen_t addrlen =
        is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_connect( sqe, pclient->fd_, addr, addrlen );
    io_uring_sqe_set_data( sqe,
                           static_cast<detail::connect_frame*>( pclient ) );

    // TODO: leaving this as `IOSQE_ASYNC` causes a leak in one of the exception
    // tests which we should fix at some point
    io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK |
                                     IOSQE_FIXED_FILE /* | IOSQE_ASYNC  */ );
  }

  {
    auto ts = &pclient->ts_;
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_link_timeout( sqe, ts, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
  }

  intrusive_ptr_add_ref( pstream_.get() );
  pclient->connect_frame::h_ = h;
  pclient->connect_frame::initiated_ = true;

  return true;
}

result<void>
connect_awaitable::await_resume()
{
  auto pclient = static_cast<detail::client_impl*>( pstream_.get() );

  auto& socket_frame = *static_cast<detail::socket_frame*>( pclient );
  auto& connect_frame = *static_cast<detail::connect_frame*>( pclient );

  if ( socket_frame.res_ < 0 ) {
    // BOOST_ASSERT( connect_frame.initiated_ );
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
