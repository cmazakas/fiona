#ifndef FIONA_TCP_HPP
#define FIONA_TCP_HPP

#include <fiona/error.hpp>      // for result
#include <fiona/io_context.hpp> // for executor

#include <fiona/detail/time.hpp> // for duration_to_timespec

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <chrono>    // for duration
#include <coroutine> // for coroutine_handle
#include <cstdint>   // for uint16_t

namespace fiona {
namespace tcp {

namespace detail {
struct acceptor_impl;
struct client_impl;
struct stream_impl;

void
intrusive_ptr_add_ref( acceptor_impl* pacceptor ) noexcept;

void
intrusive_ptr_release( acceptor_impl* pacceptor ) noexcept;

void
intrusive_ptr_add_ref( stream_impl* pstream ) noexcept;

void
intrusive_ptr_release( stream_impl* pstream ) noexcept;

void
intrusive_ptr_add_ref( client_impl* pclient ) noexcept;

void
intrusive_ptr_release( client_impl* pclient ) noexcept;

} // namespace detail

struct accept_awaitable;
struct connect_awaitable;
struct stream_cancel_awaitable;
struct stream_close_awaitable;
} // namespace tcp
} // namespace fiona

struct __kernel_timespec;
struct in6_addr;
struct in_addr;
struct sockaddr;

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

  acceptor( executor ex, in_addr ipv4_addr, std::uint16_t const port );
  acceptor( executor ex, in_addr ipv4_addr, std::uint16_t const port,
            int const backlog );

  acceptor( executor ex, in6_addr ipv6_addr, std::uint16_t const port );
  acceptor( executor ex, in6_addr ipv6_addr, std::uint16_t const port,
            int const backlog );

  bool operator==( acceptor const& ) const = default;

  std::uint16_t port() const noexcept;
  executor get_executor() const noexcept;

  accept_awaitable async_accept();
};

struct stream {
private:
  friend struct accept_awaitable;

  boost::intrusive_ptr<detail::stream_impl> pstream_;
  stream( executor ex, int fd );

public:
  stream() = delete;

  stream( stream const& ) = default;
  stream& operator=( stream const& ) = default;

  stream( stream&& ) = default;
  stream& operator=( stream&& ) = default;

  bool operator==( stream const& ) const = default;

  stream_close_awaitable async_close();
  stream_cancel_awaitable async_cancel();
};

struct stream_close_awaitable {
private:
  friend struct stream;
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_;

  stream_close_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  ~stream_close_awaitable();

  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<void> await_resume();
};

struct stream_cancel_awaitable {
private:
  friend struct stream;
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_;

  stream_cancel_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<int> await_resume();
};

struct client {
private:
  boost::intrusive_ptr<detail::client_impl> pclient_ = nullptr;

  client( boost::intrusive_ptr<detail::client_impl> pclient );

  void timeout( __kernel_timespec ts );

public:
  client() = delete;
  client( executor ex );

  client( client const& ) = default;
  client& operator=( client const& ) = default;

  client( client&& ) = default;
  client& operator=( client&& ) = default;

  bool operator==( client const& ) const = default;

  connect_awaitable async_connect( in_addr const ipv4_addr,
                                   std::uint16_t const port );
  connect_awaitable async_connect( sockaddr const* addr );

  template <class Rep, class Period>
  void timeout( std::chrono::duration<Rep, Period> const d ) {
    auto ts = fiona::detail::duration_to_timespec( d );
    timeout( ts );
  }

  executor get_executor() const noexcept;

  stream_close_awaitable async_close();
  stream_cancel_awaitable async_cancel();
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
  ~accept_awaitable();

  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<stream> await_resume();
};

struct connect_awaitable {
private:
  friend struct client;

  boost::intrusive_ptr<detail::client_impl> pclient_ = nullptr;

  connect_awaitable( boost::intrusive_ptr<detail::client_impl> pclient );

public:
  ~connect_awaitable();

  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<void> await_resume();
};

} // namespace tcp
} // namespace fiona

#endif // FIONA_TCP_HPP