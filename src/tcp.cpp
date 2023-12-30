// clang-format off
#include <fiona/tcp.hpp>
#include <fiona/error.hpp>                    // for throw_errno_as_error_code, error_code, result
#include <fiona/executor.hpp>                 // for executor, executor_access_policy

#include <fiona/detail/awaitable_base.hpp>    // for awaitable_base
#include <fiona/detail/get_sqe.hpp>           // for reserve_sqes, get_sqe

#include <boost/assert.hpp>                   // for BOOST_ASSERT
#include <boost/config/detail/suffix.hpp>     // for BOOST_NOINLINE, BOOST_NORETURN
#include <boost/smart_ptr/intrusive_ptr.hpp>  // for intrusive_ptr

#include <coroutine>                          // for coroutine_handle
#include <cstdint>                            // for uint16_t
#include <cstring>                            // for memcpy

#include <arpa/inet.h>                        // for ntohs
#include <errno.h>                            // for errno, EBUSY, EINVAL, EISCONN
#include <liburing.h>                         // for io_uring_sqe_set_data, io_uring_get_sqe, io_uring_sqe_set_flags
#include <liburing/io_uring.h>                // for io_uring_cqe, IOSQE_CQE_SKIP_SUCCESS, IOSQE_IO_LINK, IOSQE_FIXE...
#include <linux/time_types.h>                 // for __kernel_timespec
#include <netinet/in.h>                       // for sockaddr_in, sockaddr_in6, in_addr, in6_addr, IPPROTO_TCP
#include <sys/socket.h>                       // for AF_INET, AF_INET6, sockaddr_storage, sockaddr, bind, getsockname
#include <unistd.h>                           // for close
// clang-format on

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
        fiona::detail::executor_access_policy::release_fd( pacceptor_->ex_,
                                                           peer_fd_ );
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

  acceptor_impl( executor ex, sockaddr const* addr, int const backlog )
      : ex_{ ex } {
    auto const addrlen = addr->sa_family == AF_INET6 ? sizeof( sockaddr_in6 )
                                                     : sizeof( sockaddr_in );

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
                       backlog ) {}

  acceptor_impl( executor ex, sockaddr_in6 const addr, int const backlog )
      : acceptor_impl( ex, reinterpret_cast<sockaddr const*>( &addr ),
                       backlog ) {}

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

} // namespace detail

namespace detail {

struct stream_impl {
  using clock_type = std::chrono::steady_clock;
  using timepoint_type = std::chrono::time_point<clock_type>;

  struct nop_frame final : public fiona::detail::awaitable_base {
    nop_frame( stream_impl* pstream ) : pstream_{ pstream } {}
    ~nop_frame() override {}

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
    ~cancel_frame() override {}

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

    ~send_frame() override {}

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
    std::deque<result<borrowed_buffer>> buffers_;
    fiona::detail::buf_ring* pbuf_ring_ = nullptr;
    stream_impl* pstream_ = nullptr;
    std::coroutine_handle<> h_ = nullptr;
    timepoint_type last_recv_ = clock_type::now();
    int res_ = 0;
    std::uint16_t buffer_group_id_ = -1;
    bool initiated_ = false;
    bool done_ = false;

    recv_frame( stream_impl* pstream ) : pstream_{ pstream } {}

    ~recv_frame() override {}

    void await_process_cqe( io_uring_cqe* cqe ) override {
      bool const cancelled_by_timer =
          ( cqe->res == -ECANCELED && !pstream_->cancelled_ );

      if ( cqe->res < 0 ) {
        BOOST_ASSERT( !( cqe->flags & IORING_CQE_F_MORE ) );
        if ( cancelled_by_timer ) {
          buffers_.push_back( error_code::from_errno( ETIMEDOUT ) );
        } else {
          buffers_.push_back( error_code::from_errno( -cqe->res ) );
        }
      }

      if ( cqe->res == 0 ) {
        BOOST_ASSERT( !( cqe->flags & IORING_CQE_F_MORE ) );
        buffers_.push_back( borrowed_buffer() );
      }

      if ( cqe->res > 0 ) {
        BOOST_ASSERT( cqe->flags & IORING_CQE_F_BUFFER );

        // TODO: find out if we should potentially set this when we see the EOF
        last_recv_ = clock_type::now();

        auto buffer_id = cqe->flags >> 16;
        auto buffer = pbuf_ring_->get_buffer_view( buffer_id );

        buffers_.push_back( borrowed_buffer( pbuf_ring_->get(), buffer.data(),
                                             buffer.size(), pbuf_ring_->size(),
                                             buffer_id, cqe->res ) );
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
        sqe->buf_group = buffer_group_id_;
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
    virtual ~timeout_frame() override {}

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

  stream_impl( executor ex, int fd ) : stream_impl( ex ) { fd_ = fd; }

  virtual ~stream_impl() {
    if ( fd_ >= 0 ) {
      auto ring = fiona::detail::executor_access_policy::ring( ex_ );
      auto sqe = fiona::detail::get_sqe( ring );
      io_uring_prep_close_direct( sqe, fd_ );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      io_uring_sqe_set_data( sqe, nullptr );
      fiona::detail::submit_ring( ring );

      fiona::detail::executor_access_policy::release_fd( ex_, fd_ );
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

accept_awaitable::accept_awaitable(
    boost::intrusive_ptr<detail::acceptor_impl> pacceptor )
    : pacceptor_{ pacceptor } {}

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

stream::stream( executor ex, int fd )
    : pstream_{ new detail::stream_impl{ ex, fd } } {}

void
stream::timeout( __kernel_timespec ts ) {
  pstream_->ts_ = ts;

  auto ex = pstream_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  fiona::detail::reserve_sqes( ring, 2 );

  {
    auto sqe = io_uring_get_sqe( ring );

    io_uring_prep_timeout_remove(
        sqe, reinterpret_cast<std::uintptr_t>( &pstream_->timeout_frame_ ), 0 );
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
      auto user_data =
          reinterpret_cast<std::uintptr_t>( &pstream_->timeout_frame_ );
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout_remove( sqe, user_data, 0 );
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
  return async_send( std::span{
      reinterpret_cast<unsigned char const*>( msg.data() ), msg.size() } );
}

send_awaitable
stream::async_send( std::span<unsigned char const> buf ) {
  return { buf, pstream_ };
}

receiver
stream::get_receiver( std::uint16_t buffer_group_id ) {
  return { pstream_, buffer_group_id };
}

stream_close_awaitable::stream_close_awaitable(
    boost::intrusive_ptr<detail::stream_impl> pstream )
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

stream_cancel_awaitable::stream_cancel_awaitable(
    boost::intrusive_ptr<detail::stream_impl> pstream )
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
  io_uring_prep_cancel_fd(
      sqe, fd, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED );
  io_uring_sqe_set_data(
      sqe, boost::intrusive_ptr( &pstream_->cancel_frame_ ).detach() );

  cf.initiated_ = true;
  cf.h_ = h;
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

send_awaitable::send_awaitable(
    std::span<unsigned char const> buf,
    boost::intrusive_ptr<detail::stream_impl> pstream )
    : buf_{ buf }, pstream_{ pstream } {}

send_awaitable::~send_awaitable() {
  if ( pstream_->send_frame_.initiated_ && !pstream_->send_frame_.done_ ) {
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

receiver::receiver( boost::intrusive_ptr<detail::stream_impl> pstream,
                    std::uint16_t buffer_group_id )
    : pstream_{ pstream }, buffer_group_id_{ buffer_group_id } {
  pstream_->recv_frame_.buffer_group_id_ = buffer_group_id;
}

receiver::~receiver() {
  if ( pstream_->recv_frame_.initiated_ ) {
    pstream_->cancelled_ = true;
    auto ring = fiona::detail::executor_access_policy::ring( pstream_->ex_ );
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, &pstream_->recv_frame_, 0 );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    fiona::detail::submit_ring( ring );

    pstream_->recv_frame_.buffer_group_id_ = -1;
  }
}

recv_awaitable
receiver::async_recv() {
  return { pstream_ };
}

recv_awaitable::recv_awaitable(
    boost::intrusive_ptr<detail::stream_impl> pstream )
    : pstream_{ pstream } {}

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
    return;
  }

  rf.pbuf_ring_ = fiona::detail::executor_access_policy::get_buffer_group(
      pstream_->ex_, pstream_->recv_frame_.buffer_group_id_ );
  rf.schedule_recv();
}

result<borrowed_buffer>
recv_awaitable::await_resume() {
  auto buf = std::move( pstream_->recv_frame_.buffers_.front() );
  pstream_->recv_frame_.buffers_.pop_front();
  return buf;
}

namespace detail {
struct client_impl : public stream_impl {
  struct socket_frame final : public fiona::detail::awaitable_base {
    client_impl* pclient_ = nullptr;
    std::coroutine_handle<> h_;
    int res_ = 0;
    bool initiated_ = false;
    bool done_ = false;

    socket_frame( client_impl* pclient ) : pclient_{ pclient } {}

    virtual ~socket_frame() override {}

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

    virtual ~connect_frame() override {}

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

  client_impl( executor ex ) : stream_impl{ ex } {}
  ~client_impl() {}
};

void
intrusive_ptr_add_ref( client_impl* pclient ) noexcept {
  ++pclient->count_;
}

void
intrusive_ptr_release( client_impl* pclient ) noexcept {
  --pclient->count_;
  if ( pclient->count_ == 0 ) {
    delete pclient;
  }
}
} // namespace detail

client::client( executor ex ) : pclient_{ new detail::client_impl{ ex } } {}

void
client::timeout( __kernel_timespec ts ) {
  pclient_->ts_ = ts;
}

void
client::cancel_timer() {
  if ( pclient_ && pclient_->timeout_frame_.initiated_ ) {
    pclient_->timeout_frame_.cancelled_ = true;
    auto ring = fiona::detail::executor_access_policy::ring( pclient_->ex_ );
    fiona::detail::reserve_sqes( ring, 1 );
    {
      auto user_data =
          reinterpret_cast<std::uintptr_t>( &pclient_->timeout_frame_ );
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_timeout_remove( sqe, user_data, 0 );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      fiona::detail::submit_ring( ring );
    }
  }
}

executor
client::get_executor() const noexcept {
  return pclient_->ex_;
}

stream_close_awaitable
client::async_close() {
  return { pclient_ };
}

stream_cancel_awaitable
client::async_cancel() {
  return { pclient_ };
}

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

  std::memcpy( &pclient_->addr_storage_, addr,
               is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 ) );
  return { pclient_ };
}

send_awaitable
client::async_send( std::string_view msg ) {
  return async_send( std::span{
      reinterpret_cast<unsigned char const*>( msg.data() ), msg.size() } );
}

send_awaitable
client::async_send( std::span<unsigned char const> buf ) {
  return { buf, pclient_ };
}

receiver
client::get_receiver( std::uint16_t buffer_group_id ) {
  return { pclient_, buffer_group_id };
}

connect_awaitable::connect_awaitable(
    boost::intrusive_ptr<detail::client_impl> pclient )
    : pclient_{ pclient } {}

connect_awaitable::~connect_awaitable() {
  auto ex = pclient_->ex_;
  auto& sf = pclient_->socket_frame_;
  auto& cf = pclient_->connect_frame_;

  auto ring = fiona::detail::executor_access_policy::ring( ex );

  auto reserve_size =
      static_cast<int>( sf.initiated_ ) + static_cast<int>( cf.initiated_ );
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
  if ( pclient_->socket_frame_.initiated_ ) {
    throw_busy();
  }
  return false;
}

void
connect_awaitable::await_suspend( std::coroutine_handle<> h ) {
  BOOST_ASSERT( !pclient_->socket_frame_.initiated_ );
  BOOST_ASSERT( !pclient_->connect_frame_.initiated_ );
  BOOST_ASSERT( !pclient_->socket_frame_.h_ );
  BOOST_ASSERT( !pclient_->connect_frame_.h_ );

  auto const* addr = &pclient_->addr_storage_;
  auto const is_ipv4 = ( addr->ss_family == AF_INET );
  BOOST_ASSERT( is_ipv4 || addr->ss_family == AF_INET6 );

  auto af = is_ipv4 ? AF_INET : AF_INET6;
  auto ex = pclient_->ex_;
  auto ring = fiona::detail::executor_access_policy::ring( ex );

  if ( pclient_->fd_ >= 0 && pclient_->connected_ ) {
    fiona::detail::reserve_sqes( ring, 3 );
  } else {
    if ( pclient_->fd_ == -1 ) {
      auto const file_idx =
          fiona::detail::executor_access_policy::get_available_fd( ex );

      pclient_->fd_ = file_idx;
    }

    fiona::detail::reserve_sqes( ring, 4 );

    {
      auto sqe = io_uring_get_sqe( ring );
      io_uring_prep_socket_direct( sqe, af, SOCK_STREAM, IPPROTO_TCP,
                                   pclient_->fd_, 0 );
      io_uring_sqe_set_data( sqe, &pclient_->socket_frame_ );
      io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK );

      intrusive_ptr_add_ref( pclient_.get() );
    }

    pclient_->socket_frame_.h_ = h;
    pclient_->socket_frame_.initiated_ = true;
  }

  {
    auto addr = reinterpret_cast<sockaddr const*>( &pclient_->addr_storage_ );
    auto addrlen = ( is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 ) );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_connect( sqe, pclient_->fd_, addr, addrlen );
    io_uring_sqe_set_data( sqe, &pclient_->connect_frame_ );
    io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK | IOSQE_FIXED_FILE |
                                     IOSQE_CQE_SKIP_SUCCESS );
  }

  {
    auto ts = &pclient_->ts_;
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_link_timeout( sqe, ts, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS );
  }

  {
    auto addr = reinterpret_cast<sockaddr const*>( &pclient_->addr_storage_ );
    auto addrlen = ( is_ipv4 ? sizeof( sockaddr_in ) : sizeof( sockaddr_in6 ) );
    auto sqe = io_uring_get_sqe( ring );
    io_uring_prep_connect( sqe, pclient_->fd_, addr, addrlen );
    io_uring_sqe_set_data( sqe, &pclient_->connect_frame_ );
    io_uring_sqe_set_flags( sqe, IOSQE_FIXED_FILE );
  }

  intrusive_ptr_add_ref( pclient_.get() );
  pclient_->connect_frame_.h_ = h;
  pclient_->connect_frame_.initiated_ = true;
}

result<void>
connect_awaitable::await_resume() {
  auto& socket_frame = pclient_->socket_frame_;
  auto& connect_frame = pclient_->connect_frame_;

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
