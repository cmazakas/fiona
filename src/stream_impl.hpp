#ifndef FIONA_SRC_STREAM_IMPL
#define FIONA_SRC_STREAM_IMPL

#include <fiona/borrowed_buffer.hpp>
#include <fiona/executor.hpp>

#include <fiona/detail/get_sqe.hpp>

#include <chrono>

#include <liburing.h>

#include "awaitable_base.hpp"
#include "fiona/detail/common.hpp"
#include "fiona/error.hpp"

namespace fiona {
namespace tcp {
namespace detail {

struct stream_impl;

void FIONA_DECL
intrusive_ptr_add_ref( stream_impl* pstream ) noexcept;

void FIONA_DECL
intrusive_ptr_release( stream_impl* pstream ) noexcept;

struct FIONA_DECL stream_impl {
  using clock_type = std::chrono::steady_clock;
  using timepoint_type = std::chrono::time_point<clock_type>;

  struct nop_frame final : public fiona::detail::awaitable_base {
    nop_frame( stream_impl* pstream ) : pstream_{ pstream } {}
    ~nop_frame() override;

    stream_impl* pstream_ = nullptr;

    void await_process_cqe( io_uring_cqe* cqe ) override { (void)cqe; }
    std::coroutine_handle<> handle() noexcept override { return nullptr; }
    void inc_ref() noexcept override { ++pstream_->count_; }
    void dec_ref() noexcept override {
      if ( --pstream_->count_ == 0 ) {
        delete pstream_;
      }
    }

    int use_count() const noexcept override { return pstream_->count_; }
  };

  struct cancel_frame : public fiona::detail::awaitable_base {
    stream_impl* pstream_ = nullptr;
    std::coroutine_handle<> h_ = nullptr;
    int res_ = 0;
    bool initiated_ = false;
    bool done_ = false;

    cancel_frame() = delete;
    cancel_frame( stream_impl* pstream ) : pstream_{ pstream } {}
    ~cancel_frame() override;

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
      if ( h_ ) {
        auto h = h_;
        h_ = nullptr;
        return h;
      }
      return nullptr;
    }

    void inc_ref() noexcept override { ++pstream_->count_; }
    void dec_ref() noexcept override {
      if ( --pstream_->count_ == 0 ) {
        delete pstream_;
      }
    }

    int use_count() const noexcept override { return pstream_->count_; }
  };

  struct close_frame : public cancel_frame {
    ~close_frame() override;
    using cancel_frame::cancel_frame;

    void await_process_cqe( io_uring_cqe* cqe ) override {
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
  };

  struct send_frame final : public fiona::detail::awaitable_base {
    stream_impl* pstream_ = nullptr;
    std::coroutine_handle<> h_ = nullptr;
    timepoint_type last_send_ = clock_type::now();
    int res_ = 0;
    bool initiated_ = false;
    bool done_ = false;

    send_frame( stream_impl* pstream ) : pstream_{ pstream } {}

    ~send_frame() override;

    void reset() {
      h_ = nullptr;
      res_ = 0;
      initiated_ = false;
      done_ = false;
    }

    void await_process_cqe( io_uring_cqe* cqe ) override {
      done_ = true;
      res_ = cqe->res;
      if ( res_ < 0 ) {
        pstream_->connected_ = false;
      }
    }

    std::coroutine_handle<> handle() noexcept override {
      auto h = h_;
      h_ = nullptr;
      return h;
    }

    void inc_ref() noexcept override { ++pstream_->count_; }
    void dec_ref() noexcept override {
      if ( --pstream_->count_ == 0 ) {
        delete pstream_;
      }
    }
    int use_count() const noexcept override { return pstream_->count_; }
  };

  struct recv_frame final : public fiona::detail::awaitable_base {
    fiona::recv_buffer_sequence buffers_;
    fiona::error_code ec_;
    fiona::detail::buf_ring* pbuf_ring_ = nullptr;
    stream_impl* pstream_ = nullptr;
    std::coroutine_handle<> h_ = nullptr;
    timepoint_type last_recv_ = clock_type::now();
    int res_ = 0;
    int buffer_group_id_ = -1;
    bool initiated_ = false;
    bool done_ = false;

    recv_frame( stream_impl* pstream ) : pstream_{ pstream } {}

    ~recv_frame() override;

    void await_process_cqe( io_uring_cqe* cqe ) override {
      bool const cancelled_by_timer =
          ( cqe->res == -ECANCELED && !pstream_->cancelled_ );

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
        buffers_.push_back( recv_buffer( 0 ) );
      }

      if ( cqe->res > 0 ) {
        BOOST_ASSERT( cqe->flags & IORING_CQE_F_BUFFER );

        // TODO: find out if we should potentially set this when we see the EOF
        last_recv_ = clock_type::now();

        auto buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

        auto& buf = pbuf_ring_->get_buf( buffer_id );
        auto cap = buf.capacity();

        auto buffer = std::move( buf );
        buffer.set_len( static_cast<std::size_t>( cqe->res ) );
        buffers_.push_back( std::move( buffer ) );

        // TODO: we need somehow set an upper limit on the amount of allocations
        // that can happen and then if so, how we handle back-filling the buffer
        // group's buffer list
        buf = recv_buffer( cap );

        auto* br = pbuf_ring_->get();
        io_uring_buf_ring_add(
            br, buf.data(), static_cast<unsigned>( buf.capacity() ),
            static_cast<unsigned short>( buffer_id ),
            io_uring_buf_ring_mask( pbuf_ring_->size() ), 0 );

        io_uring_buf_ring_advance( br, 1 );
      }

      if ( ( cqe->flags & IORING_CQE_F_MORE ) ) {
        intrusive_ptr_add_ref( this );
        initiated_ = true;
      } else {
        initiated_ = false;
      }
    }

    std::coroutine_handle<> handle() noexcept override {
      auto h = h_;
      h_ = nullptr;
      return h;
    }

    void inc_ref() noexcept override { ++pstream_->count_; }
    void dec_ref() noexcept override {
      if ( --pstream_->count_ == 0 ) {
        delete pstream_;
      }
    }

    int use_count() const noexcept override { return pstream_->count_; }

    void schedule_recv() {
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
  };

  struct timeout_frame final : public fiona::detail::awaitable_base {
    stream_impl* pstream_ = nullptr;
    bool initiated_ = false;
    bool done_ = false;
    bool cancelled_ = false;

    timeout_frame() = delete;
    timeout_frame( stream_impl* pstream ) : pstream_{ pstream } {}
    ~timeout_frame() override;

    void reset() {
      initiated_ = false;
      done_ = false;
      cancelled_ = false;
    }

    void await_process_cqe( io_uring_cqe* cqe ) override {
      if ( cqe->res == -ECANCELED && cancelled_ ) {
        initiated_ = false;
        return;
      }

      auto const timeout_adjusted = ( cqe->res == -ECANCELED );
      if ( timeout_adjusted ) {
        auto ring =
            fiona::detail::executor_access_policy::ring( pstream_->ex_ );

        fiona::detail::reserve_sqes( ring, 1 );

        {
          auto sqe = io_uring_get_sqe( ring );
          io_uring_prep_timeout( sqe, &pstream_->ts_, 0,
                                 IORING_TIMEOUT_MULTISHOT );
          io_uring_sqe_set_data( sqe, &pstream_->timeout_frame_ );
        }

        pstream_->timeout_frame_.initiated_ = true;
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

      if ( pstream_->recv_frame_.initiated_ ) {
        auto diff = now - pstream_->recv_frame_.last_recv_;
        if ( diff >= max_diff ) {
          fiona::detail::reserve_sqes( ring, 1 );
          auto sqe = io_uring_get_sqe( ring );
          io_uring_prep_cancel( sqe, &pstream_->recv_frame_,
                                IORING_ASYNC_CANCEL_ALL );
          io_uring_sqe_set_data( sqe, &pstream_->recv_cancel_frame_ );
          intrusive_ptr_add_ref( &pstream_->recv_cancel_frame_ );
        }
      }

      if ( pstream_->send_frame_.initiated_ && !pstream_->send_frame_.done_ ) {
        auto diff = now - pstream_->send_frame_.last_send_;
        if ( diff >= max_diff ) {
          fiona::detail::reserve_sqes( ring, 1 );
          auto sqe = io_uring_get_sqe( ring );
          io_uring_prep_cancel( sqe, &pstream_->send_frame_,
                                IORING_ASYNC_CANCEL_ALL );
          io_uring_sqe_set_data( sqe, &pstream_->send_cancel_frame_ );
          intrusive_ptr_add_ref( &pstream_->send_cancel_frame_ );
        }
      }

      if ( !( cqe->flags & IORING_CQE_F_MORE ) ) {
        fiona::detail::reserve_sqes( ring, 1 );
        {
          auto sqe = io_uring_get_sqe( ring );
          io_uring_prep_timeout( sqe, &pstream_->ts_, 0,
                                 IORING_TIMEOUT_MULTISHOT );
          io_uring_sqe_set_data( sqe, &pstream_->timeout_frame_ );
        }
        pstream_->timeout_frame_.initiated_ = true;
      }

      intrusive_ptr_add_ref( this );
    }

    std::coroutine_handle<> handle() noexcept override { return nullptr; }

    void inc_ref() noexcept override { ++pstream_->count_; }
    void dec_ref() noexcept override {
      if ( --pstream_->count_ == 0 ) {
        delete pstream_;
      }
    }

    int use_count() const noexcept override { return pstream_->count_; }
  };

  cancel_frame cancel_frame_{ this };
  close_frame close_frame_{ this };
  send_frame send_frame_{ this };
  recv_frame recv_frame_{ this };
  timeout_frame timeout_frame_{ this };
  nop_frame send_cancel_frame_{ this };
  nop_frame recv_cancel_frame_{ this };
  __kernel_timespec ts_ = { .tv_sec = 3, .tv_nsec = 0 };
  executor ex_;
  int count_ = 0;
  int fd_ = -1;
  bool connected_ = false;
  bool cancelled_ = false;

  stream_impl( executor ex ) : ex_{ ex } {
    auto ring = fiona::detail::executor_access_policy::ring( ex );

    fiona::detail::reserve_sqes( ring, 1 );

    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_timeout( sqe, &ts_, 0, IORING_TIMEOUT_MULTISHOT );
    io_uring_sqe_set_data( sqe, &timeout_frame_ );

    timeout_frame_.initiated_ = true;
    intrusive_ptr_add_ref( this );
  }

  stream_impl() = delete;
  stream_impl( stream_impl const& ) = delete;
  stream_impl( stream_impl&& ) = delete;

  stream_impl( executor ex, int fd ) : stream_impl( ex ) { fd_ = fd; }

  virtual ~stream_impl();
};

struct FIONA_DECL client_impl : public stream_impl {
  struct socket_frame final : public fiona::detail::awaitable_base {
    client_impl* pclient_ = nullptr;
    std::coroutine_handle<> h_;
    int res_ = 0;
    bool initiated_ = false;
    bool done_ = false;

    socket_frame( client_impl* pclient ) : pclient_{ pclient } {}

    ~socket_frame() override;

    void await_process_cqe( io_uring_cqe* cqe ) override {
      done_ = true;
      if ( cqe->res < 0 ) {
        res_ = cqe->res;
        fiona::detail::executor_access_policy::release_fd( pclient_->ex_,
                                                           pclient_->fd_ );
        pclient_->fd_ = -1;
      }
    }

    std::coroutine_handle<> handle() noexcept override {
      if ( res_ < 0 ) {
        auto h = h_;
        h_ = nullptr;
        return h;
      }
      return nullptr;
    }

    void inc_ref() noexcept override { ++pclient_->count_; }
    void dec_ref() noexcept override {
      --pclient_->count_;
      if ( pclient_->count_ == 0 ) {
        delete pclient_;
      }
    }
    int use_count() const noexcept override { return pclient_->count_; }

    void reset() {
      h_ = nullptr;
      res_ = 0;
      initiated_ = false;
      done_ = false;
    }
  };

  struct connect_frame final : public fiona::detail::awaitable_base {
    client_impl* pclient_ = nullptr;
    std::coroutine_handle<> h_;
    int res_ = 0;
    bool initiated_ = false;
    bool done_ = false;

    connect_frame( client_impl* pclient ) : pclient_{ pclient } {}

    ~connect_frame() override;

    void await_process_cqe( io_uring_cqe* cqe ) override {
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

    std::coroutine_handle<> handle() noexcept override {
      auto h = h_;
      h_ = nullptr;
      return h;
    }

    void inc_ref() noexcept override { ++pclient_->count_; }
    void dec_ref() noexcept override {
      --pclient_->count_;
      if ( pclient_->count_ == 0 ) {
        delete pclient_;
      }
    }
    int use_count() const noexcept override { return pclient_->count_; }

    void reset() {
      h_ = nullptr;
      res_ = 0;
      initiated_ = false;
      done_ = false;
    }
  };

  sockaddr_storage addr_storage_ = {};
  socket_frame socket_frame_{ this };
  connect_frame connect_frame_{ this };

  client_impl() = delete;
  client_impl( client_impl const& ) = delete;
  client_impl( client_impl&& ) = delete;
  client_impl( executor ex ) : stream_impl{ ex } {}
  virtual ~client_impl() override;
};

} // namespace detail
} // namespace tcp
} // namespace fiona

#endif // FIONA_SRC_STREAM_IMPL
