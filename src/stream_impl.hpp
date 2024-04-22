// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_SRC_STREAM_IMPL
#define FIONA_SRC_STREAM_IMPL

#include <fiona/borrowed_buffer.hpp>
#include <fiona/error.hpp>
#include <fiona/executor.hpp>

#include <fiona/detail/common.hpp>
#include <fiona/detail/get_sqe.hpp>

#include <chrono>

#include <liburing.h>

#include "awaitable_base.hpp"

namespace fiona {
namespace tcp {
namespace detail {

struct stream_impl;
struct client_impl;

using fiona::detail::ref_count;

using clock_type = std::chrono::steady_clock;
using timepoint_type = std::chrono::time_point<clock_type>;

struct FIONA_DECL cancel_frame : public fiona::detail::awaitable_base {
  stream_impl* pstream_ = nullptr;
  std::coroutine_handle<> h_ = nullptr;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  cancel_frame() = delete;
  cancel_frame( cancel_frame const& ) = delete;
  cancel_frame( stream_impl* pstream ) : pstream_( pstream ) {}
  virtual ~cancel_frame() override;

  void reset() {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  void await_process_cqe( io_uring_cqe* cqe ) override {
    done_ = true;
    res_ = cqe->res;
  }

  std::coroutine_handle<> handle() noexcept override {
    return boost::exchange( h_, nullptr );
  }

  bool is_active() const noexcept { return initiated_ && !done_; }
};

struct FIONA_DECL timeout_cancel_frame : public fiona::detail::awaitable_base {
  stream_impl* pstream_ = nullptr;
  int res_ = 0;
  bool initiated_ = false;

  timeout_cancel_frame() = delete;
  timeout_cancel_frame( timeout_cancel_frame const& ) = delete;
  timeout_cancel_frame( stream_impl* pstream ) : pstream_( pstream ) {}
  virtual ~timeout_cancel_frame() override;

  void await_process_cqe( io_uring_cqe* cqe ) override {
    initiated_ = false;
    res_ = cqe->res;
  }

  std::coroutine_handle<> handle() noexcept override { return nullptr; }

  bool is_active() const noexcept { return initiated_; }
};

struct FIONA_DECL close_frame : public fiona::detail::awaitable_base {
  stream_impl* pstream_ = nullptr;
  std::coroutine_handle<> h_ = nullptr;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  close_frame() = delete;
  close_frame( stream_impl* pstream ) : pstream_( pstream ) {}
  virtual ~close_frame() override;

  void reset() {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  std::coroutine_handle<> handle() noexcept override {
    return boost::exchange( h_, nullptr );
  }

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  bool is_active() const noexcept { return initiated_ && !done_; }
};

struct FIONA_DECL send_frame : public fiona::detail::awaitable_base {
  stream_impl* pstream_ = nullptr;
  std::coroutine_handle<> h_ = nullptr;
  timepoint_type last_send_ = clock_type::now();
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  send_frame( stream_impl* pstream ) : pstream_( pstream ) {}

  ~send_frame() override;

  void reset() {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<> handle() noexcept override {
    return boost::exchange( h_, nullptr );
  }

  bool is_active() const noexcept { return initiated_ && !done_; }
};

struct FIONA_DECL recv_frame : public fiona::detail::awaitable_base {
  fiona::recv_buffer_sequence buffers_;
  fiona::error_code ec_;
  fiona::detail::buf_ring* pbuf_ring_ = nullptr;
  stream_impl* pstream_ = nullptr;
  std::coroutine_handle<> h_ = nullptr;
  timepoint_type last_recv_ = clock_type::now();
  int res_ = 0;
  int buffer_group_id_ = -1;
  bool initiated_ = false;

  recv_frame( stream_impl* pstream ) : pstream_( pstream ) {}

  virtual ~recv_frame() override;

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<> handle() noexcept override {
    return boost::exchange( h_, nullptr );
  }

  inline void schedule_recv();
  bool is_active() const noexcept { return initiated_; }
};

struct FIONA_DECL timeout_frame : public fiona::detail::awaitable_base {
  stream_impl* pstream_ = nullptr;
  bool initiated_ = false;
  bool cancelled_ = false;

  timeout_frame() = delete;
  timeout_frame( stream_impl* pstream ) : pstream_( pstream ) {}
  virtual ~timeout_frame() override;

  void reset() {
    initiated_ = false;
    cancelled_ = false;
  }

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<> handle() noexcept override { return nullptr; }
  bool is_active() const noexcept { return initiated_; }
};

struct FIONA_DECL stream_impl : public virtual ref_count,
                                public cancel_frame,
                                public close_frame,
                                public send_frame,
                                public recv_frame,
                                public timeout_frame,
                                public timeout_cancel_frame {

  __kernel_timespec ts_ = { .tv_sec = 3, .tv_nsec = 0 };
  executor ex_;
  int fd_ = -1;
  bool connected_ = false;
  bool stream_cancelled_ = false;

  stream_impl( executor ex )
      : cancel_frame( this ), close_frame( this ), send_frame( this ),
        recv_frame( this ), timeout_frame( this ), timeout_cancel_frame( this ),
        ex_( ex ) {

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
  stream_impl( stream_impl&& ) = delete;

  stream_impl( executor ex, int fd ) : stream_impl( ex ) { fd_ = fd; }

  virtual ~stream_impl() override;
};

void
detail::close_frame::await_process_cqe( io_uring_cqe* cqe ) {
  done_ = true;
  res_ = cqe->res;
  if ( res_ >= 0 ) {
    auto ex = pstream_->ex_;
    auto fd = pstream_->fd_;
    fiona::detail::executor_access_policy::release_fd( ex, fd );
    pstream_->fd_ = -1;
    pstream_->connected_ = false;
  }
}

void
detail::send_frame::await_process_cqe( io_uring_cqe* cqe ) {
  done_ = true;
  res_ = cqe->res;
  if ( res_ < 0 ) {
    pstream_->connected_ = false;
  }
}

void
detail::recv_frame::await_process_cqe( io_uring_cqe* cqe ) {
  bool const cancelled_by_timer =
      ( cqe->res == -ECANCELED && !pstream_->stream_cancelled_ );

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
detail::recv_frame::schedule_recv() {
  auto ex = pstream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto fd = pstream_->fd_;

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
detail::timeout_frame::await_process_cqe( io_uring_cqe* cqe ) {
  if ( cqe->res == -ECANCELED && cancelled_ ) {
    initiated_ = false;
    return;
  }

  auto const timeout_adjusted = ( cqe->res == -ECANCELED );
  if ( timeout_adjusted ) {
    auto ring = fiona::detail::executor_access_policy::ring( pstream_->ex_ );

    fiona::detail::reserve_sqes( ring, 1 );

    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout( sqe, &pstream_->ts_, 0, IORING_TIMEOUT_MULTISHOT );
      io_uring_sqe_set_data( sqe,
                             static_cast<detail::timeout_frame*>( pstream_ ) );
    }

    pstream_->timeout_frame::initiated_ = true;
    intrusive_ptr_add_ref( this );
    return;
  }

  if ( cqe->res != -ETIME && cqe->res != 0 ) {
    BOOST_ASSERT( false );
    initiated_ = false;
    return;
  }

  auto ex = pstream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );
  auto now = clock_type::now();
  auto max_diff = std::chrono::seconds{ pstream_->ts_.tv_sec } +
                  std::chrono::nanoseconds{ pstream_->ts_.tv_nsec };

  bool should_cancel = false;
  if ( pstream_->recv_frame::initiated_ ) {
    auto diff = now - pstream_->recv_frame::last_recv_;
    if ( diff >= max_diff ) {
      should_cancel = true;
    }
  }

  if ( pstream_->send_frame::initiated_ && !pstream_->send_frame::done_ ) {
    auto diff = now - pstream_->send_frame::last_send_;
    if ( diff >= max_diff ) {
      should_cancel = true;
    }
  }

  if ( should_cancel ) {
    auto fd = pstream_->fd_;
    BOOST_ASSERT( fd >= 0 );
    fiona::detail::reserve_sqes( ring, 1 );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_cancel_fd(
        sqe, fd, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED );
    io_uring_sqe_set_data(
        sqe, static_cast<detail::timeout_cancel_frame*>( pstream_ ) );

    intrusive_ptr_add_ref( pstream_ );

    pstream_->timeout_cancel_frame::initiated_ = true;
  }

  if ( !( cqe->flags & IORING_CQE_F_MORE ) ) {
    fiona::detail::reserve_sqes( ring, 1 );
    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout( sqe, &pstream_->ts_, 0, IORING_TIMEOUT_MULTISHOT );
      io_uring_sqe_set_data( sqe,
                             static_cast<detail::timeout_frame*>( pstream_ ) );
    }
    pstream_->timeout_frame::initiated_ = true;
  }

  intrusive_ptr_add_ref( this );
}

struct FIONA_DECL socket_frame : public fiona::detail::awaitable_base {
  client_impl* pclient_ = nullptr;
  std::coroutine_handle<> h_;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  socket_frame( client_impl* pclient ) : pclient_( pclient ) {}

  ~socket_frame() override;

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<> handle() noexcept override {
    if ( res_ < 0 ) {
      return boost::exchange( h_, nullptr );
    }
    // we don't currently skip successful CQEs
    // TODO: at some point we might wanna look into that
    return nullptr;
  }

  void reset() {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  bool is_active() const noexcept { return initiated_ && !done_; }
};

struct FIONA_DECL connect_frame : public fiona::detail::awaitable_base {
  client_impl* pclient_ = nullptr;
  std::coroutine_handle<> h_;
  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  connect_frame( client_impl* pclient ) : pclient_( pclient ) {}

  ~connect_frame() override;

  inline void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<> handle() noexcept override {
    return boost::exchange( h_, nullptr );
  }

  void reset() {
    h_ = nullptr;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  bool is_active() const noexcept { return initiated_ && !done_; }
};

struct FIONA_DECL client_impl : public stream_impl,
                                public socket_frame,
                                public connect_frame {
  sockaddr_storage addr_storage_ = {};

  client_impl() = delete;
  client_impl( client_impl const& ) = delete;
  client_impl( client_impl&& ) = delete;
  client_impl( executor ex )
      : stream_impl( ex ), socket_frame( this ), connect_frame( this ) {}
  virtual ~client_impl() override;
};

void
detail::socket_frame::await_process_cqe( io_uring_cqe* cqe ) {
  done_ = true;
  if ( cqe->res < 0 ) {
    res_ = cqe->res;
    fiona::detail::executor_access_policy::release_fd( pclient_->ex_,
                                                       pclient_->fd_ );
    pclient_->fd_ = -1;
  }
}

void
detail::connect_frame::await_process_cqe( io_uring_cqe* cqe ) {
  done_ = true;
  if ( cqe->res < 0 ) {
    res_ = cqe->res;
    if ( cqe->res != -EISCONN ) {
      pclient_->connected_ = false;
    }
  } else {
    pclient_->connected_ = true;
  }
}

} // namespace detail
} // namespace tcp
} // namespace fiona

#endif // FIONA_SRC_STREAM_IMPL
