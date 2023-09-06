#ifndef FIONA_TCP_HPP
#define FIONA_TCP_HPP

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

#include <thread>

namespace fiona {
namespace tcp {

struct borrowed_buffer {
private:
  io_uring_buf_ring* br_ = nullptr;
  void* addr_ = nullptr;
  unsigned len_ = 0;
  unsigned num_read_ = 0;
  std::size_t num_bufs_ = 0;
  std::uint16_t bid_ = 0;

public:
  borrowed_buffer() = default;

  borrowed_buffer( borrowed_buffer const& ) = delete;
  borrowed_buffer& operator=( borrowed_buffer const& ) = delete;

  borrowed_buffer( io_uring_buf_ring* br, void* addr, unsigned len,
                   std::size_t num_bufs, std::uint16_t bid, unsigned num_read )
      : br_( br ), addr_( addr ), len_{ len }, num_read_{ num_read },
        num_bufs_{ num_bufs }, bid_{ bid } {}

  borrowed_buffer( borrowed_buffer&& rhs ) noexcept
      : br_( rhs.br_ ), addr_( rhs.addr_ ), len_{ rhs.len_ },
        num_read_{ rhs.num_read_ }, num_bufs_{ rhs.num_bufs_ },
        bid_{ rhs.bid_ } {
    rhs.br_ = nullptr;
    rhs.addr_ = nullptr;
    rhs.len_ = 0;
    rhs.num_read_ = 0;
    rhs.num_bufs_ = 0;
    rhs.bid_ = 0;
  }

  ~borrowed_buffer() {
    if ( br_ ) {
      auto buf_ring = br_;
      io_uring_buf_ring_add( buf_ring, addr_, len_, bid_,
                             io_uring_buf_ring_mask( num_bufs_ ), 0 );
      io_uring_buf_ring_advance( buf_ring, 1 );
    }
  }

  borrowed_buffer& operator=( borrowed_buffer&& rhs ) noexcept {
    if ( this != &rhs ) {
      if ( br_ ) {
        auto buf_ring = br_;
        io_uring_buf_ring_add( buf_ring, addr_, len_, bid_,
                               io_uring_buf_ring_mask( num_bufs_ ), 0 );
        io_uring_buf_ring_advance( buf_ring, 1 );
      }

      br_ = rhs.br_;
      addr_ = rhs.addr_;
      len_ = rhs.len_;
      num_read_ = rhs.num_read_;
      num_bufs_ = rhs.num_bufs_;
      bid_ = rhs.bid_;

      rhs.br_ = nullptr;
      rhs.addr_ = nullptr;
      rhs.len_ = 0;
      rhs.num_read_ = 0;
      rhs.num_bufs_ = 0;
      rhs.bid_ = 0;
    }
    return *this;
  }

  std::span<unsigned char> readable_bytes() const noexcept {
    return { static_cast<unsigned char*>( addr_ ), num_read_ };
  }
};

struct stream {
private:
  struct recv_awaitable {
    friend struct stream;

  private:
    struct frame final : public fiona::detail::awaitable_base {
      std::deque<result<borrowed_buffer>> buffers_;

      std::coroutine_handle<> h_;
      io_uring* ring_ = nullptr;

      fiona::buf_ring& br_;

      int fd_ = -1;
      int res_ = 0;
      std::uint16_t bgid_ = 0;

      bool initiated_ = false;

      frame( io_uring* ring, fiona::buf_ring& br, int fd, std::uint16_t bgid )
          : ring_( ring ), br_( br ), fd_{ fd }, bgid_{ bgid } {}
      virtual ~frame() {}

      void await_process_cqe( io_uring_cqe* cqe ) {
        if ( cqe->res >= 0 ) {
          if ( cqe->flags & IORING_CQE_F_BUFFER ) {
            std::uint16_t bid = cqe->flags >> 16;
            auto buf = br_.get_buffer( bid );
            buffers_.push_back( borrowed_buffer( br_.get(), buf.data(),
                                                 buf.size(), br_.size(), bid,
                                                 cqe->res ) );
          } else {
            buffers_.push_back( borrowed_buffer{} );
          }
        } else {
          buffers_.push_back( fiona::error_code::from_errno( -cqe->res ) );
        }

        if ( !( cqe->flags & IORING_CQE_F_MORE ) ) {
          initiated_ = false;
        }

        if ( ( cqe->flags & IORING_CQE_F_MORE ) /*  && ( cqe->res >= 0 ) */ ) {
          intrusive_ptr_add_ref( this );
        }
      }

      std::coroutine_handle<> handle() noexcept {
        auto h = h_;
        h_ = nullptr;
        return h;
      }
    };

    recv_awaitable( io_uring* ring, fiona::buf_ring& br, int fd,
                    std::uint16_t bgid )
        : p_( new frame( ring, br, fd, bgid ) ) {}

    boost::intrusive_ptr<frame> p_;

  public:
    ~recv_awaitable() {
      auto& self = *p_;
      if ( self.initiated_ ) {
        auto ring = self.ring_;
        auto sqe = fiona::detail::get_sqe( ring );
        io_uring_prep_cancel( sqe, p_.get(), 0 );
        io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
        io_uring_sqe_set_data( sqe, nullptr );
        io_uring_submit( ring );
      }
    }

    bool await_ready() noexcept {
      auto& self = *p_;
      return !self.buffers_.empty();
    }

    void await_suspend( std::coroutine_handle<> h ) {
      auto& self = *p_;
      self.h_ = h;
      if ( self.initiated_ ) {
        return;
      }

      auto ring = self.ring_;
      auto sqe = fiona::detail::get_sqe( ring );
      io_uring_prep_recv_multishot( sqe, self.fd_, nullptr, 0, 0 );
      io_uring_sqe_set_flags( sqe, IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE );
      io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
      sqe->buf_group = self.bgid_;

      self.initiated_ = true;
    }

    result<borrowed_buffer> await_resume() {
      auto& self = *p_;
      BOOST_ASSERT( !self.buffers_.empty() );
      auto borrowed_buf = std::move( self.buffers_.front() );
      self.buffers_.pop_front();

      return borrowed_buf;
    }
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
    ~write_awaitable() {
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

    bool await_ready() { return false; }
    void await_suspend( std::coroutine_handle<> h ) {
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
        io_uring_prep_send( sqe, self.fd_, self.buf_, self.nbytes_,
                            MSG_WAITALL );
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
  __kernel_timespec ts_;
  int fd_ = -1;

public:
  stream() = delete;

  stream( stream const& ) = delete;
  stream& operator=( stream const& ) = delete;

  stream( fiona::executor ex, int fd ) : ex_( ex ), ts_{}, fd_{ fd } {}

  stream( stream&& rhs ) noexcept
      : ex_( std::move( rhs.ex_ ) ), ts_{ rhs.ts_ }, fd_{ rhs.fd_ } {
    rhs.ts_ = {};
    rhs.fd_ = -1;
  }

  ~stream() {
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

  stream& operator=( stream&& rhs ) noexcept {
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
    auto maybe_group =
        detail::executor_access_policy::get_buffer_group( ex_, bgid );

    if ( !maybe_group ) {
      fiona::detail::throw_errno_as_error_code( EINVAL );
    }

    return recv_awaitable( detail::executor_access_policy::ring( ex_ ),
                           *maybe_group, fd_, bgid );
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

    void await_suspend( std::coroutine_handle<> h ) {
      auto& self = *p_;
      if ( self.initiated_ ) {
        return;
      }

      auto ring = self.ring_;
      auto sqe = fiona::detail::get_sqe( ring );
      auto file_idx =
          detail::executor_access_policy::get_available_fd( self.ex_ );

      io_uring_prep_accept_direct( sqe, self.fd_, nullptr, nullptr, 0,
                                   file_idx );
      io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );

      self.peer_fd_ = file_idx;
      self.h_ = h;
      self.initiated_ = true;
    }

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

  acceptor& operator=( acceptor&& rhs ) noexcept {
    if ( this != &rhs ) {
      memcpy( &addr_storage_, &rhs.addr_storage_, sizeof( addr_storage_ ) );

      ex_ = std::move( rhs.ex_ );

      fd_ = rhs.fd_;
      rhs.fd_ = -1;

      is_ipv4_ = rhs.is_ipv4_;
    }

    return *this;
  }

  ~acceptor() {
    if ( fd_ != -1 ) {
      close( fd_ );
    }
  }

  acceptor( fiona::executor ex, std::uint32_t ipv4_addr, std::uint16_t port )
      : ex_( ex ) {

    memset( &addr_storage_, 0, sizeof( addr_storage_ ) );

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

    constexpr int backlog = 5000;
    if ( -1 == listen( fd, backlog ) ) {
      fiona::detail::throw_errno_as_error_code( errno );
    }

    fd_ = fd;
    is_ipv4_ = true;
    if ( port == 0 ) {
      socklen_t addrlen = sizeof( sockaddr_in );
      if ( -1 == getsockname( fd_,
                              reinterpret_cast<sockaddr*>( &addr_storage_ ),
                              &addrlen ) ) {
        fiona::detail::throw_errno_as_error_code( errno );
      }
    } else {
      memcpy( &addr_storage_, &addr, sizeof( addr ) );
    }
  }

  accept_awaitable async_accept() {
    return { ex_, detail::executor_access_policy::ring( ex_ ), fd_ };
  }

  std::uint16_t port() {
    // current limitation because of wsl2's lack of ipv6
    BOOST_ASSERT( is_ipv4_ );
    auto paddr = reinterpret_cast<sockaddr_in const*>( &addr_storage_ );
    return ntohs( paddr->sin_port );
  }
};

struct client : public stream {
private:
  friend struct client_awaitable;

  struct client_awaitable {
    friend struct client;

    struct frame final : public fiona::detail::awaitable_base {
      executor ex_;
      std::coroutine_handle<> h_;
      int fd_ = -1;
      bool initiated_ = false;
      bool done_ = false;

      frame( executor ex ) : ex_( ex ) {}

      void await_process_cqe( io_uring_cqe* cqe ) {
        if ( cqe->res < 0 ) {
          detail::executor_access_policy::release_fd( ex_, fd_ );
          fd_ = cqe->res;
        }
        done_ = true;
      }

      std::coroutine_handle<> handle() noexcept { return h_; }
    };

    boost::intrusive_ptr<frame> p_;

    client_awaitable( executor ex ) : p_( new frame( ex ) ) {}

    ~client_awaitable() {
      auto& self = *p_;
      if ( self.initiated_ && !self.done_ ) {
        auto ring = detail::executor_access_policy::ring( self.ex_ );
        auto sqe = fiona::detail::get_sqe( ring );
        io_uring_prep_cancel( sqe, p_.get(), 0 );
        io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
        io_uring_sqe_set_data( sqe, nullptr );
        io_uring_submit( ring );
      }
    }

    bool await_ready() const noexcept { return false; }
    void await_suspend( std::coroutine_handle<> h ) {
      auto& self = *p_;
      if ( self.initiated_ ) {
        return;
      }

      self.h_ = h;

      auto ring = detail::executor_access_policy::ring( self.ex_ );
      auto sqe = fiona::detail::get_sqe( ring );
      auto file_idx =
          detail::executor_access_policy::get_available_fd( self.ex_ );

      self.fd_ = file_idx;

      io_uring_prep_socket_direct( sqe, AF_INET, SOCK_STREAM, 0, file_idx, 0 );
      io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
      self.initiated_ = true;
    }

    result<client> await_resume() {
      auto& self = *p_;
      if ( self.fd_ < 0 ) {
        return fiona::error_code::from_errno( -self.fd_ );
      }
      return { client( self.ex_, self.fd_ ) };
    }
  };

  struct connect_awaitable {
    friend struct client;

  private:
    struct frame final : public fiona::detail::awaitable_base {
      enum state { uninit, socket_created, connected, done };

      __kernel_timespec ts_;
      sockaddr_storage addr_;
      executor ex_;
      std::coroutine_handle<> h_;
      int fd_ = -1;
      int res_ = 0;
      state s_ = state::uninit;
      bool initiated_ = false;
      bool done_ = false;

      void await_process_cqe( io_uring_cqe* cqe ) {
        res_ = cqe->res;

        switch ( s_ ) {
        case state::uninit: {
          s_ = socket_created;
          if ( res_ < 0 ) {
            s_ = state::done;
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
          throw;
        }
      }

      std::coroutine_handle<> handle() noexcept {
        if ( !done_ ) {
          return nullptr;
        }
        return h_;
      }

      frame( sockaddr_in ipv4_addr, __kernel_timespec ts, executor ex )
          : ts_{ ts }, ex_( ex ) {
        memcpy( &addr_, &ipv4_addr, sizeof( ipv4_addr ) );
      }
    };

    boost::intrusive_ptr<frame> p_;

    connect_awaitable( sockaddr_in ipv4_addr, __kernel_timespec ts,
                       executor ex )
        : p_( new frame( ipv4_addr, ts, ex ) ) {}

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

    bool await_ready() { return false; }
    void await_suspend( std::coroutine_handle<> h ) {
      auto& self = *p_;
      if ( self.initiated_ ) {
        return;
      }

      auto ring = detail::executor_access_policy::ring( self.ex_ );
      // socket() -> connect() + timeout
      //
      detail::reserve_sqes( ring, 3 );

      {
        auto sqe = fiona::detail::get_sqe( ring );
        auto file_idx =
            detail::executor_access_policy::get_available_fd( self.ex_ );

        self.fd_ = file_idx;

        io_uring_prep_socket_direct( sqe, AF_INET, SOCK_STREAM, 0, self.fd_,
                                     0 );
        io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
        io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK );
      }

      {
        auto sqe = fiona::detail::get_sqe( ring );
        io_uring_prep_connect( sqe, self.fd_,
                               reinterpret_cast<sockaddr const*>( &self.addr_ ),
                               sizeof( self.addr_ ) );
        io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK | IOSQE_FIXED_FILE );
        io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
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

  static client_awaitable make( executor ex ) { return client_awaitable( ex ); }

  static connect_awaitable async_connect( executor ex, std::uint32_t ipv4_addr,
                                          std::uint16_t port ) {
    return async_connect( ex, ipv4_addr, port, std::chrono::seconds( 3 ) );
  }

  template <class Rep, class Period>
  static connect_awaitable
  async_connect( executor ex, std::uint32_t ipv4_addr, std::uint16_t port,
                 std::chrono::duration<Rep, Period> d ) {
    auto ts = fiona::detail::duration_to_timespec( d );

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons( port );
    addr.sin_addr.s_addr = htonl( ipv4_addr );
    return { addr, ts, ex };
  }
};

} // namespace tcp
} // namespace fiona

#endif // FIONA_TCP_HPP
