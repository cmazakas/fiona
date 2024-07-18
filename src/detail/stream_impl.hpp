// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_SRC_STREAM_IMPL
#define FIONA_SRC_STREAM_IMPL

#include <fiona/error.hpp>
#include <fiona/executor.hpp>

#include <fiona/detail/common.hpp>
#include <fiona/detail/get_sqe.hpp>

#include <chrono>

#include <liburing.h>

#include "awaitable_base.hpp"
#include <fiona_export.h>

namespace fiona {
namespace tcp {
namespace detail {

struct stream_impl;
struct client_impl;

using fiona::detail::ref_count;

using clock_type = std::chrono::steady_clock;
using timepoint_type = std::chrono::time_point<clock_type>;

struct FIONA_EXPORT cancel_frame : public fiona::detail::awaitable_base
{
  std::coroutine_handle<> h_ = nullptr;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  cancel_frame() = default;

  cancel_frame( cancel_frame const& ) = delete;
  ~cancel_frame() override;

  void
  reset()
  {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  void
  await_process_cqe( io_uring_cqe* cqe ) override
  {
    done_ = true;
    res_ = cqe->res;
  }

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }

  bool
  is_active() const noexcept
  {
    return initiated_ && !done_;
  }
};

struct FIONA_EXPORT timeout_cancel_frame : public fiona::detail::awaitable_base
{
  int res_ = 0;
  bool initiated_ = false;

  timeout_cancel_frame() = default;

  timeout_cancel_frame( timeout_cancel_frame const& ) = delete;
  timeout_cancel_frame& operator=( timeout_cancel_frame const& ) = delete;

  ~timeout_cancel_frame() override;

  void
  await_process_cqe( io_uring_cqe* cqe ) override
  {
    initiated_ = false;
    res_ = cqe->res;
  }

  std::coroutine_handle<>
  handle() noexcept override
  {
    return nullptr;
  }

  bool
  is_active() const noexcept
  {
    return initiated_;
  }
};

struct FIONA_EXPORT close_frame : public fiona::detail::awaitable_base
{
  std::coroutine_handle<> h_ = nullptr;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  close_frame() = default;

  close_frame( close_frame const& ) = delete;
  close_frame& operator=( close_frame const& ) = delete;

  ~close_frame() override;

  void
  reset()
  {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  bool
  is_active() const noexcept
  {
    return initiated_ && !done_;
  }
};

struct FIONA_EXPORT shutdown_frame : public fiona::detail::awaitable_base
{
  std::coroutine_handle<> h_ = nullptr;
  int how_ = 0;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  shutdown_frame() = default;

  shutdown_frame( shutdown_frame const& ) = delete;
  shutdown_frame& operator=( shutdown_frame const& ) = delete;

  ~shutdown_frame() override;

  void
  reset()
  {
    h_ = nullptr;
    how_ = 0;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }

  void
  await_process_cqe( io_uring_cqe* cqe ) override
  {
    done_ = true;
    res_ = cqe->res;
  }

  bool
  is_active() const noexcept
  {
    return initiated_ && !done_;
  }
};

struct FIONA_EXPORT send_frame : public fiona::detail::awaitable_base
{
  std::coroutine_handle<> h_ = nullptr;
  timepoint_type last_send_ = clock_type::now();
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  send_frame() = default;

  send_frame( send_frame const& ) = delete;
  send_frame& operator=( send_frame const& ) = delete;

  ~send_frame() override;

  void
  reset()
  {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }

  bool
  is_active() const noexcept
  {
    return initiated_ && !done_;
  }
};

struct FIONA_EXPORT recv_frame : public fiona::detail::awaitable_base
{
  fiona::recv_buffer_sequence buffers_;
  fiona::error_code ec_;
  fiona::detail::buf_ring* pbuf_ring_ = nullptr;
  std::coroutine_handle<> h_ = nullptr;
  timepoint_type last_recv_ = clock_type::now();
  int res_ = 0;
  int buffer_group_id_ = -1;
  bool initiated_ = false;

  recv_frame() = default;

  recv_frame( recv_frame const& ) = delete;
  recv_frame& operator=( recv_frame const& ) = delete;

  ~recv_frame() override;

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }

  inline void schedule_recv();
  bool
  is_active() const noexcept
  {
    return initiated_;
  }
};

struct FIONA_EXPORT timeout_frame : public fiona::detail::awaitable_base
{
  bool initiated_ = false;
  bool cancelled_ = false;

  timeout_frame() = default;

  timeout_frame( timeout_frame const& ) = delete;
  timeout_frame& operator=( timeout_frame const& ) = delete;

  ~timeout_frame() override;

  void
  reset()
  {
    initiated_ = false;
    cancelled_ = false;
  }

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<>
  handle() noexcept override
  {
    return nullptr;
  }

  bool
  is_active() const noexcept
  {
    return initiated_;
  }
};

struct FIONA_EXPORT stream_impl : public virtual ref_count,
                                  public cancel_frame,
                                  public close_frame,
                                  public shutdown_frame,
                                  public send_frame,
                                  public recv_frame,
                                  public timeout_frame,
                                  public timeout_cancel_frame
{
  __kernel_timespec ts_ = { .tv_sec = 3, .tv_nsec = 0 };
  executor ex_;
  int fd_ = -1;
  bool connected_ = false;
  bool stream_cancelled_ = false;

  stream_impl( executor ex ) : ex_( ex )
  {

    auto ring = fiona::detail::executor_access_policy::ring( ex );

    fiona::detail::reserve_sqes( ring, 1 );

    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_timeout( sqe, &ts_, 0, IORING_TIMEOUT_MULTISHOT );
    io_uring_sqe_set_data( sqe, static_cast<timeout_frame*>( this ) );

    this->timeout_frame::initiated_ = true;
    intrusive_ptr_add_ref( this );
  }

  stream_impl() = delete;
  stream_impl( stream_impl const& ) = delete;
  stream_impl& operator=( stream_impl const& ) = delete;

  stream_impl( executor ex, int fd ) : stream_impl( ex ) { fd_ = fd; }

  virtual ~stream_impl() override;
};

void
detail::close_frame::await_process_cqe( io_uring_cqe* cqe )
{
  auto& stream = static_cast<detail::stream_impl&>( *this );

  done_ = true;
  res_ = cqe->res;
  if ( res_ >= 0 ) {
    auto ex = stream.ex_;
    auto fd = stream.fd_;
    fiona::detail::executor_access_policy::release_fd( ex, fd );
    stream.fd_ = -1;
    stream.connected_ = false;
  }
}

void
detail::send_frame::await_process_cqe( io_uring_cqe* cqe )
{
  done_ = true;
  res_ = cqe->res;
  if ( res_ < 0 ) {
    static_cast<detail::stream_impl*>( this )->connected_ = false;
  }
}

void
detail::recv_frame::await_process_cqe( io_uring_cqe* cqe )
{
  auto& stream = static_cast<detail::stream_impl&>( *this );

  bool const cancelled_by_timer =
      ( cqe->res == -ECANCELED && !stream.stream_cancelled_ );

  if ( cqe->res < 0 ) {
    BOOST_ASSERT( !( cqe->flags & IORING_CQE_F_MORE ) );
    BOOST_ASSERT( !ec_ );
    if ( cancelled_by_timer ) {
      ec_ = error_code::from_errno( ETIMEDOUT );
    } else {
      ec_ = error_code::from_errno( -cqe->res );
    }
  }

  if ( cqe->res == 0 ) {
    BOOST_ASSERT( !( cqe->flags & IORING_CQE_F_MORE ) );
    BOOST_ASSERT( !( cqe->flags & IORING_CQE_F_BUFFER ) );
    buffers_.push_back( recv_buffer( 0 ) );
  }

  if ( cqe->res > 0 ) {
    BOOST_ASSERT( cqe->flags & IORING_CQE_F_BUFFER );

    // TODO: find out if we should potentially set this when we see the EOF
    last_recv_ = clock_type::now();

    auto buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

    auto& buf = pbuf_ring_->get_buf( buffer_id );
    BOOST_ASSERT( buf.capacity() );
    auto buffer = std::move( buf );
    buffer.set_len( static_cast<std::size_t>( cqe->res ) );
    buffers_.push_back( std::move( buffer ) );

    *pbuf_ring_->buf_id_pos_++ = buffer_id;
  }

  if ( ( cqe->flags & IORING_CQE_F_MORE ) ) {
    intrusive_ptr_add_ref( this );
    initiated_ = true;
  } else {
    initiated_ = false;
  }
}

void
detail::recv_frame::schedule_recv()
{
  auto& stream = static_cast<detail::stream_impl&>( *this );
  auto ex = stream.ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto fd = stream.fd_;

  fiona::detail::reserve_sqes( ring, 1 );

  {
    auto flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_recv_multishot( sqe, fd, nullptr, 0, 0 );
    io_uring_sqe_set_data( sqe, this );
    io_uring_sqe_set_flags( sqe, flags );
    sqe->ioprio |= IORING_RECVSEND_POLL_FIRST;
    sqe->buf_group = static_cast<unsigned short>( buffer_group_id_ );
  }

  initiated_ = true;
  last_recv_ = clock_type::now();
  intrusive_ptr_add_ref( this );
}

void
detail::timeout_frame::await_process_cqe( io_uring_cqe* cqe )
{
  auto& stream = static_cast<detail::stream_impl&>( *this );

  if ( cqe->res == -ECANCELED && cancelled_ ) {
    initiated_ = false;
    return;
  }

  auto const timeout_adjusted = ( cqe->res == -ECANCELED );
  if ( timeout_adjusted ) {
    auto ring = fiona::detail::executor_access_policy::ring( stream.ex_ );

    fiona::detail::reserve_sqes( ring, 1 );

    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout( sqe, &stream.ts_, 0, IORING_TIMEOUT_MULTISHOT );
      io_uring_sqe_set_data( sqe,
                             static_cast<detail::timeout_frame*>( &stream ) );
    }

    stream.timeout_frame::initiated_ = true;
    intrusive_ptr_add_ref( this );
    return;
  }

  if ( cqe->res != -ETIME && cqe->res != 0 ) {
    BOOST_ASSERT( false );
    initiated_ = false;
    return;
  }

  auto ex = stream.ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto now = clock_type::now();
  auto max_diff = std::chrono::seconds{ stream.ts_.tv_sec } +
                  std::chrono::nanoseconds{ stream.ts_.tv_nsec };

  bool should_cancel = false;
  if ( stream.recv_frame::initiated_ ) {
    auto diff = now - stream.recv_frame::last_recv_;
    if ( diff >= max_diff ) {
      should_cancel = true;
    }
  }

  if ( stream.send_frame::initiated_ && !stream.send_frame::done_ ) {
    auto diff = now - stream.send_frame::last_send_;
    if ( diff >= max_diff ) {
      should_cancel = true;
    }
  }

  if ( should_cancel ) {
    auto fd = stream.fd_;
    BOOST_ASSERT( fd >= 0 );
    fiona::detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel_fd(
        sqe, fd, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED );
    io_uring_sqe_set_data(
        sqe, static_cast<detail::timeout_cancel_frame*>( &stream ) );

    intrusive_ptr_add_ref( &stream );

    stream.timeout_cancel_frame::initiated_ = true;
  }

  if ( !( cqe->flags & IORING_CQE_F_MORE ) ) {
    fiona::detail::reserve_sqes( ring, 1 );
    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout( sqe, &stream.ts_, 0, IORING_TIMEOUT_MULTISHOT );
      io_uring_sqe_set_data( sqe,
                             static_cast<detail::timeout_frame*>( &stream ) );
    }
    stream.timeout_frame::initiated_ = true;
  }

  intrusive_ptr_add_ref( this );
}

struct FIONA_EXPORT socket_frame : public fiona::detail::awaitable_base
{
  std::coroutine_handle<> h_;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  socket_frame() = default;

  socket_frame( socket_frame const& ) = delete;
  socket_frame& operator=( socket_frame const& ) = delete;

  ~socket_frame() override;

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<>
  handle() noexcept override
  {
    if ( res_ < 0 ) {
      return boost::exchange( h_, nullptr );
    }
    // we don't currently skip successful CQEs
    // TODO: at some point we might wanna look into that
    return nullptr;
  }

  void
  reset()
  {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  bool
  is_active() const noexcept
  {
    return initiated_ && !done_;
  }
};

struct FIONA_EXPORT connect_frame : public fiona::detail::awaitable_base
{
  std::coroutine_handle<> h_;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  connect_frame() = default;

  connect_frame( connect_frame const& ) = delete;
  connect_frame& operator=( connect_frame const& ) = delete;

  ~connect_frame() override;

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }

  void
  reset()
  {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  bool
  is_active() const noexcept
  {
    return initiated_ && !done_;
  }
};

struct FIONA_EXPORT client_impl : public stream_impl,
                                  public socket_frame,
                                  public connect_frame
{
  sockaddr_storage addr_storage_ = {};

  client_impl() = delete;
  client_impl( client_impl const& ) = delete;
  client_impl( client_impl&& ) = delete;
  client_impl( executor ex ) : stream_impl( ex ) {}
  virtual ~client_impl() override;
};

void
detail::socket_frame::await_process_cqe( io_uring_cqe* cqe )
{
  auto& client = static_cast<detail::client_impl&>( *this );
  done_ = true;
  if ( cqe->res < 0 ) {
    res_ = cqe->res;
    fiona::detail::executor_access_policy::release_fd( client.ex_, client.fd_ );
    client.fd_ = -1;
  }
}

void
detail::connect_frame::await_process_cqe( io_uring_cqe* cqe )
{
  auto& client = static_cast<detail::client_impl&>( *this );
  done_ = true;
  if ( cqe->res < 0 ) {
    res_ = cqe->res;
    if ( cqe->res != -EISCONN ) {
      client.connected_ = false;
    }
  } else {
    client.connected_ = true;
  }
}

} // namespace detail
} // namespace tcp
} // namespace fiona

#endif // FIONA_SRC_STREAM_IMPL
