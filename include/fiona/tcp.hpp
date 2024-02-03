#ifndef FIONA_TCP_HPP
#define FIONA_TCP_HPP

#include <fiona/borrowed_buffer.hpp>         // for borrowed_buffer
#include <fiona/error.hpp>                   // for result
#include <fiona/executor.hpp>                // for executor

#include <fiona/detail/config.hpp>           // for FIONA_DECL
#include <fiona/detail/time.hpp>             // for duration_to_timespec

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <chrono>                            // for duration
#include <coroutine>                         // for coroutine_handle
#include <cstddef>                           // for size_t
#include <cstdint>                           // for uint16_t
#include <span>                              // for span
#include <string_view>                       // for string_view

namespace fiona {
namespace tcp {

namespace detail {

struct acceptor_impl;
struct client_impl;
struct stream_impl;

} // namespace detail

struct accept_awaitable;
struct connect_awaitable;
struct stream_cancel_awaitable;
struct stream_close_awaitable;
struct send_awaitable;
struct recv_awaitable;

struct receiver;

} // namespace tcp
} // namespace fiona

struct __kernel_timespec;
struct sockaddr;
struct sockaddr_in;
struct sockaddr_in6;

namespace fiona {
namespace tcp {
namespace detail {

void FIONA_DECL
intrusive_ptr_add_ref( acceptor_impl* pacceptor ) noexcept;

void FIONA_DECL
intrusive_ptr_release( acceptor_impl* pacceptor ) noexcept;

void FIONA_DECL
intrusive_ptr_add_ref( stream_impl* pstream ) noexcept;

void FIONA_DECL
intrusive_ptr_release( stream_impl* pstream ) noexcept;

void FIONA_DECL
intrusive_ptr_add_ref( client_impl* pclient ) noexcept;

void FIONA_DECL
intrusive_ptr_release( client_impl* pclient ) noexcept;

} // namespace detail
} // namespace tcp
} // namespace fiona

namespace fiona {
namespace tcp {

struct acceptor {
private:
  boost::intrusive_ptr<detail::acceptor_impl> pacceptor_;

public:
  acceptor() = delete;

  acceptor( acceptor const& ) = default;
  acceptor& operator=( acceptor const& ) = default;

  acceptor( acceptor&& ) = default;
  acceptor& operator=( acceptor&& ) = default;

  FIONA_DECL
  acceptor( executor ex, sockaddr const* addr );

  acceptor( executor ex, sockaddr_in const* addr ) : acceptor( ex, reinterpret_cast<sockaddr const*>( addr ) ) {}
  acceptor( executor ex, sockaddr_in6 const* addr ) : acceptor( ex, reinterpret_cast<sockaddr const*>( addr ) ) {}

  bool operator==( acceptor const& ) const = default;

  FIONA_DECL
  std::uint16_t port() const noexcept;

  FIONA_DECL
  executor get_executor() const noexcept;

  FIONA_DECL
  accept_awaitable async_accept();
};

struct stream {
private:
  friend struct accept_awaitable;

  boost::intrusive_ptr<detail::stream_impl> pstream_;
  stream( executor ex, int fd );

  FIONA_DECL
  void timeout( __kernel_timespec ts );

  FIONA_DECL
  void cancel_timer();

public:
  stream() = delete;

  stream( stream const& ) = default;
  stream& operator=( stream const& ) = default;

  stream( stream&& ) = default;
  stream& operator=( stream&& ) = default;

  ~stream() { cancel_timer(); }

  bool operator==( stream const& ) const = default;

  FIONA_DECL
  executor get_executor() const;

  template <class Rep, class Period>
  void timeout( std::chrono::duration<Rep, Period> const d ) {
    auto ts = fiona::detail::duration_to_timespec( d );
    timeout( ts );
  }

  FIONA_DECL
  stream_close_awaitable async_close();

  FIONA_DECL
  stream_cancel_awaitable async_cancel();

  FIONA_DECL
  send_awaitable async_send( std::string_view msg );

  FIONA_DECL
  send_awaitable async_send( std::span<unsigned char const> buf );

  FIONA_DECL
  receiver get_receiver( std::uint16_t buffer_group_id );
};

struct stream_close_awaitable {
private:
  friend struct stream;
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_;

  stream_close_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  FIONA_DECL
  ~stream_close_awaitable();

  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<void> await_resume();
};

struct stream_cancel_awaitable {
private:
  friend struct stream;
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_;

  stream_cancel_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<int> await_resume();
};

struct send_awaitable {
private:
  friend struct stream;
  friend struct client;

  std::span<unsigned char const> buf_;
  boost::intrusive_ptr<detail::stream_impl> pstream_ = nullptr;

  send_awaitable( std::span<unsigned char const> buf, boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  FIONA_DECL
  ~send_awaitable();

  bool await_ready() const noexcept { return false; }

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<std::size_t> await_resume();
};

struct receiver {
private:
  friend struct stream;
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_ = nullptr;
  std::uint16_t buffer_group_id_ = -1;

  receiver( boost::intrusive_ptr<detail::stream_impl> pstream, std::uint16_t buffer_group_id );

public:
  receiver() = delete;

  receiver( receiver const& ) = delete;
  receiver& operator=( receiver const& ) = delete;

  receiver( receiver&& ) = delete;
  receiver& operator=( receiver&& ) = delete;

  FIONA_DECL
  ~receiver();

  FIONA_DECL
  recv_awaitable async_recv();
};

struct recv_awaitable {
private:
  friend struct receiver;

  boost::intrusive_ptr<detail::stream_impl> pstream_ = nullptr;

  recv_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  recv_awaitable() = delete;

  recv_awaitable( recv_awaitable const& ) = delete;
  recv_awaitable& operator=( recv_awaitable const& ) = delete;

  recv_awaitable( recv_awaitable&& ) = delete;
  recv_awaitable& operator=( recv_awaitable&& ) = delete;

  FIONA_DECL
  ~recv_awaitable();

  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<borrowed_buffer> await_resume();
};

struct client {
private:
  boost::intrusive_ptr<detail::client_impl> pclient_ = nullptr;

  client( boost::intrusive_ptr<detail::client_impl> pclient );

  FIONA_DECL
  void timeout( __kernel_timespec ts );

  FIONA_DECL
  void cancel_timer();

public:
  client() = delete;

  FIONA_DECL
  client( executor ex );

  client( client const& ) = default;
  client& operator=( client const& ) = default;

  client( client&& ) = default;
  client& operator=( client&& ) = default;

  ~client() { cancel_timer(); }

  bool operator==( client const& ) const = default;

  FIONA_DECL
  connect_awaitable async_connect( sockaddr const* addr );

  FIONA_DECL
  connect_awaitable async_connect( sockaddr_in const* addr );

  FIONA_DECL
  connect_awaitable async_connect( sockaddr_in6 const* addr );

  template <class Rep, class Period>
  void timeout( std::chrono::duration<Rep, Period> const d ) {
    auto ts = fiona::detail::duration_to_timespec( d );
    timeout( ts );
  }

  FIONA_DECL
  executor get_executor() const noexcept;

  FIONA_DECL
  stream_close_awaitable async_close();

  FIONA_DECL
  stream_cancel_awaitable async_cancel();

  FIONA_DECL
  send_awaitable async_send( std::string_view msg );

  FIONA_DECL
  send_awaitable async_send( std::span<unsigned char const> buf );

  FIONA_DECL
  receiver get_receiver( std::uint16_t buffer_group_id );
};

struct accept_awaitable {
private:
  friend struct acceptor;

  boost::intrusive_ptr<detail::acceptor_impl> pacceptor_ = nullptr;

  accept_awaitable( boost::intrusive_ptr<detail::acceptor_impl> pacceptor );

public:
  accept_awaitable() = delete;
  accept_awaitable( accept_awaitable const& ) = delete;
  accept_awaitable( accept_awaitable&& ) = delete;

  FIONA_DECL
  ~accept_awaitable();

  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<stream> await_resume();
};

struct connect_awaitable {
private:
  friend struct client;

  boost::intrusive_ptr<detail::client_impl> pclient_ = nullptr;

  connect_awaitable( boost::intrusive_ptr<detail::client_impl> pclient );

public:
  FIONA_DECL
  ~connect_awaitable();

  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<void> await_resume();
};

} // namespace tcp
} // namespace fiona

#endif // FIONA_TCP_HPP
