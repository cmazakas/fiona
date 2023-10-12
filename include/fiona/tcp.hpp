#ifndef FIONA_TCP_HPP
#define FIONA_TCP_HPP

#include <fiona/borrowed_buffer.hpp>
#include <fiona/detail/awaitable_base.hpp>
#include <fiona/detail/get_sqe.hpp>
#include <fiona/detail/time.hpp>
#include <fiona/error.hpp>
#include <fiona/io_context.hpp>

#include <boost/assert.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <iostream>

#include <liburing.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>

#include <chrono>

namespace fiona {
namespace tcp {

struct stream {
private:
  struct recv_awaitable {
    friend struct stream;

  private:
    using clock_type = std::chrono::steady_clock;
    using timepoint_type = std::chrono::time_point<clock_type>;

    struct frame final : public fiona::detail::awaitable_base {
      std::deque<result<borrowed_buffer>> buffers_;
      timepoint_type last_activity_ = clock_type::now();
      __kernel_timespec ts_;
      executor ex_;
      std::coroutine_handle<> h_;
      fiona::buf_ring& br_;
      int fd_ = -1;
      int res_ = 0;
      std::uint16_t bgid_ = 0;
      bool initiated_ = false;
      bool canceled_ = false;
      bool was_rescheduled_ = false;

      frame() = delete;
      frame( frame&& ) = delete;
      frame( frame const& ) = delete;
      frame( __kernel_timespec ts, executor ex, fiona::buf_ring& br, int fd,
             std::uint16_t bgid )
          : ts_{ ts }, ex_{ ex }, br_{ br }, fd_{ fd }, bgid_{ bgid } {}

      virtual ~frame() {}

      void schedule_recv();

      void await_process_cqe( io_uring_cqe* cqe );

      std::coroutine_handle<> handle() noexcept {
        if ( was_rescheduled_ ) {
          was_rescheduled_ = false;
          return nullptr;
        }

        auto h = h_;
        h_ = nullptr;
        return h;
      }
    };

    recv_awaitable( __kernel_timespec ts, executor ex, fiona::buf_ring& br,
                    int fd, std::uint16_t bgid )
        : p_( new frame( ts, ex, br, fd, bgid ) ) {}

    boost::intrusive_ptr<frame> p_;

  public:
    ~recv_awaitable();

    bool await_ready() noexcept {
      auto& self = *p_;
      auto is_ready = !self.buffers_.empty();
      return is_ready;
    }

    void await_suspend( std::coroutine_handle<> h );
    result<borrowed_buffer> await_resume();
  };

  struct write_awaitable {
    friend struct stream;

  private:
    struct frame final : public fiona::detail::awaitable_base {
      __kernel_timespec ts_;
      io_uring* ring_ = nullptr;
      void const* buf_ = nullptr;
      std::coroutine_handle<> h_ = nullptr;
      unsigned nbytes_ = 0;
      int fd_ = -1;
      int res_ = 0;
      bool initiated_ = false;
      bool done_ = false;

      virtual ~frame() {}

      frame() = delete;
      frame( frame&& ) = delete;
      frame( frame const& ) = delete;
      frame( __kernel_timespec ts, io_uring* ring, int fd, void const* buf,
             unsigned nbytes )
          : ts_{ ts }, ring_( ring ), buf_( buf ), nbytes_{ nbytes },
            fd_{ fd } {}

      void await_process_cqe( io_uring_cqe* cqe ) {
        res_ = cqe->res;
        done_ = true;
      }

      std::coroutine_handle<> handle() noexcept { return h_; }
    };

  private:
    boost::intrusive_ptr<frame> p_;

    write_awaitable( __kernel_timespec ts, io_uring* ring, int fd,
                     void const* buf, unsigned nbytes )
        : p_( new frame( ts, ring, fd, buf, nbytes ) ) {}

  public:
    ~write_awaitable();

    bool await_ready() { return false; }
    void await_suspend( std::coroutine_handle<> h );

    fiona::result<std::size_t> await_resume() {
      auto& self = *p_;
      if ( self.res_ >= 0 ) {
        return { static_cast<std::size_t>( self.res_ ) };
      }
      return fiona::error_code::from_errno( -self.res_ );
    }
  };

protected:
  fiona::executor ex_;
  __kernel_timespec ts_ = __kernel_timespec{ .tv_sec = 3, .tv_nsec = 0 };
  int fd_ = -1;

public:
  stream() = delete;

  stream( stream const& ) = delete;
  stream& operator=( stream const& ) = delete;

  stream( fiona::executor ex, int fd ) : ex_( ex ), fd_{ fd } {}

  stream( stream&& rhs ) noexcept
      : ex_( std::move( rhs.ex_ ) ), ts_{ rhs.ts_ }, fd_{ rhs.fd_ } {
    rhs.ts_ = {};
    rhs.fd_ = -1;
  }

  ~stream();
  stream& operator=( stream&& rhs ) noexcept;

  template <class Rep, class Period>
  void timeout( std::chrono::duration<Rep, Period> const& d ) {
    ts_ = fiona::detail::duration_to_timespec( d );
  }

  __kernel_timespec timeout() const noexcept { return ts_; }

  write_awaitable async_write( void const* buf, unsigned nbytes ) {
    return write_awaitable( ts_, detail::executor_access_policy::ring( ex_ ),
                            fd_, buf, nbytes );
  }

  recv_awaitable async_recv( std::uint16_t bgid ) {
    auto pgroup = detail::executor_access_policy::get_buffer_group( ex_, bgid );
    if ( !pgroup ) {
      fiona::detail::throw_errno_as_error_code( EINVAL );
    }
    return recv_awaitable( ts_, ex_, *pgroup, fd_, bgid );
  }
};

struct acceptor {
private:
  struct accept_awaitable {
    friend struct acceptor;

  private:
    struct frame final : fiona::detail::awaitable_base {
      sockaddr_storage addr_storage_;
      socklen_t addr_storage_size_ = sizeof( addr_storage_ );
      std::deque<int> connections_;
      executor ex_;
      std::coroutine_handle<> h_;
      io_uring* ring_ = nullptr;
      int fd_ = -1;
      int peer_fd_ = -1;
      bool initiated_ = false;
      bool done_ = false;

      frame() = delete;
      frame( frame&& ) = delete;
      frame( frame const& ) = delete;
      frame( executor ex, io_uring* ring, int fd )
          : ex_( ex ), ring_( ring ), fd_{ fd } {}

      virtual ~frame() {}

      void await_process_cqe( io_uring_cqe* cqe ) {
        if ( cqe->res < 0 ) {
          detail::executor_access_policy::release_fd( ex_, peer_fd_ );
          peer_fd_ = cqe->res;
        }
        done_ = true;
      }

      std::coroutine_handle<> handle() noexcept {
        auto h = h_;
        h_ = nullptr;
        return h;
      }
    };

    accept_awaitable( executor ex, io_uring* ring, int fd )
        : p_( new frame( ex, ring, fd ) ) {}

    boost::intrusive_ptr<frame> p_;

  public:
    ~accept_awaitable() {
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

    bool await_ready() noexcept { return false; }
    void await_suspend( std::coroutine_handle<> h );

    fiona::result<stream> await_resume() {
      auto& self = *p_;

      auto fd = self.peer_fd_;
      if ( fd < 0 ) {
        return error_code::from_errno( -fd );
      }

      return stream( self.ex_, fd );
    }
  };

private:
  sockaddr_storage addr_storage_;
  fiona::executor ex_;
  int fd_ = -1;
  bool is_ipv4_ = true;

public:
  acceptor() = delete;
  acceptor( acceptor const& ) = delete;
  acceptor& operator=( acceptor const& ) = delete;

  acceptor( acceptor&& rhs ) noexcept : ex_( std::move( rhs.ex_ ) ) {
    memcpy( &addr_storage_, &rhs.addr_storage_, sizeof( addr_storage_ ) );

    fd_ = rhs.fd_;
    rhs.fd_ = -1;

    is_ipv4_ = rhs.is_ipv4_;
  }

  acceptor& operator=( acceptor&& rhs ) noexcept;

  ~acceptor() {
    if ( fd_ != -1 ) {
      close( fd_ );
    }
  }

  acceptor( fiona::executor ex, in_addr ipv4_addr, std::uint16_t port,
            int backlog );
  acceptor( fiona::executor ex, in6_addr ipv6_addr, std::uint16_t port,
            int backlog );

  acceptor( fiona::executor ex, in_addr ipv4_addr, std::uint16_t port );
  acceptor( fiona::executor ex, in6_addr ipv6_addr, std::uint16_t port );

  accept_awaitable async_accept() {
    return { ex_, detail::executor_access_policy::ring( ex_ ), fd_ };
  }

  std::uint16_t port() {
    if ( is_ipv4_ ) {
      auto paddr = reinterpret_cast<sockaddr_in const*>( &addr_storage_ );
      return ntohs( paddr->sin_port );
    }

    auto paddr = reinterpret_cast<sockaddr_in6 const*>( &addr_storage_ );
    return ntohs( paddr->sin6_port );
  }
};

struct client : public stream {
private:
  struct connect_awaitable {
    friend struct client;

  private:
    struct frame final : public fiona::detail::awaitable_base {
      enum state { uninit, socket_created };

      sockaddr_storage addr_;
      __kernel_timespec ts_;
      executor ex_;
      std::coroutine_handle<> h_;
      int fd_ = -1;
      int res_ = 0;
      state s_ = state::uninit;
      bool initiated_ = false;
      bool done_ = false;
      bool is_ipv4_ = true;

      frame() = delete;
      frame( frame&& ) = delete;
      frame( frame const& ) = delete;

      void await_process_cqe( io_uring_cqe* cqe ) {
        res_ = cqe->res;

        switch ( s_ ) {
        case state::uninit: {
          s_ = socket_created;
          if ( res_ < 0 ) {
            done_ = true;
          }
          break;
        }

        case state::socket_created: {
          if ( res_ < 0 ) {
            detail::executor_access_policy::release_fd( ex_, fd_ );
          }

          BOOST_ASSERT( use_count() == 2 );
          done_ = true;
          break;
        }

        default:
          BOOST_ASSERT( false );
          throw;
        }
      }

      std::coroutine_handle<> handle() noexcept {
        BOOST_ASSERT( h_ );
        if ( !done_ ) {
          return nullptr;
        }
        auto h = h_;
        h_ = nullptr;
        return h;
      }

      frame( sockaddr_in ipv4_addr, __kernel_timespec ts, executor ex )
          : ts_{ ts }, ex_( ex ) {
        memcpy( &addr_, &ipv4_addr, sizeof( ipv4_addr ) );
      }

      frame( sockaddr_in6 ipv6_addr, __kernel_timespec ts, executor ex )
          : ts_{ ts }, ex_( ex ) {
        memcpy( &addr_, &ipv6_addr, sizeof( ipv6_addr ) );
      }
    };

    boost::intrusive_ptr<frame> p_;

    connect_awaitable( sockaddr_in ipv4_addr, __kernel_timespec ts,
                       executor ex )
        : p_( new frame( ipv4_addr, ts, ex ) ) {}

    connect_awaitable( sockaddr_in6 ipv6_addr, __kernel_timespec ts,
                       executor ex )
        : p_( new frame( ipv6_addr, ts, ex ) ) {
      p_->is_ipv4_ = false;
    }

  public:
    ~connect_awaitable() {
      auto& self = *p_;
      if ( self.initiated_ && !self.done_ ) {
        auto ring = fiona::detail::executor_access_policy::ring( self.ex_ );
        auto sqe = fiona::detail::get_sqe( ring );
        io_uring_prep_cancel( sqe, p_.get(), 0 );
        io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
        io_uring_sqe_set_data( sqe, nullptr );
        io_uring_submit( ring );
      }
    }

    bool await_ready() {
      auto& self = *p_;
      return self.done_;
    }

    void await_suspend( std::coroutine_handle<> h );

    result<client> await_resume() {
      auto& self = *p_;
      if ( self.res_ < 0 ) {
        return fiona::error_code::from_errno( -self.res_ );
      }

      return { client( self.ex_, self.fd_ ) };
    }
  };

  client( fiona::executor ex, int fd ) : stream( ex, fd ) {}

public:
  ~client() = default;

  client( client const& ) = delete;
  client& operator=( client const& ) = delete;

  client( client&& rhs ) noexcept = default;
  client& operator=( client&& rhs ) noexcept = default;

  static connect_awaitable async_connect( executor ex, in_addr ipv4_addr,
                                          std::uint16_t port ) {
    return async_connect( ex, ipv4_addr, port, std::chrono::seconds( 3 ) );
  }

  static connect_awaitable async_connect( executor ex, in6_addr ipv6_addr,
                                          std::uint16_t port ) {
    return async_connect( ex, ipv6_addr, port, std::chrono::seconds( 3 ) );
  }

  template <class Rep, class Period>
  static connect_awaitable
  async_connect( executor ex, in_addr ipv4_addr, std::uint16_t port,
                 std::chrono::duration<Rep, Period> d ) {
    auto ts = fiona::detail::duration_to_timespec( d );

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons( port );
    addr.sin_addr.s_addr = htonl( ipv4_addr.s_addr );
    return { addr, ts, ex };
  }

  template <class Rep, class Period>
  static connect_awaitable
  async_connect( executor ex, in6_addr ipv6_addr, std::uint16_t port,
                 std::chrono::duration<Rep, Period> d ) {
    auto ts = fiona::detail::duration_to_timespec( d );

    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons( port );
    addr.sin6_addr = ipv6_addr;
    return { addr, ts, ex };
  }
};

} // namespace tcp
} // namespace fiona

#endif // FIONA_TCP_HPP
