#ifndef FIONA_TCP_HPP
#define FIONA_TCP_HPP

#include <fiona/error.hpp>
#include <fiona/io_context.hpp>

#include <fiona/detail/time.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <coroutine>

#include <netinet/in.h>

namespace fiona {

namespace detail {

struct acceptor_impl;

void
intrusive_ptr_add_ref( acceptor_impl* pacceptor ) noexcept;

void
intrusive_ptr_release( acceptor_impl* pacceptor ) noexcept;

struct stream_impl;

void
intrusive_ptr_add_ref( stream_impl* pstream ) noexcept;

void
intrusive_ptr_release( stream_impl* pstream ) noexcept;

struct client_impl;

void
intrusive_ptr_add_ref( client_impl* pclient ) noexcept;

void
intrusive_ptr_release( client_impl* pclient ) noexcept;

} // namespace detail

struct accept_awaitable;

struct acceptor {
private:
  boost::intrusive_ptr<detail::acceptor_impl> pacceptor_;

public:
  acceptor() = delete;

  acceptor( acceptor const& ) = delete;
  acceptor& operator=( acceptor const& ) = delete;

  acceptor( acceptor&& ) = default;
  acceptor& operator=( acceptor&& ) = default;

  acceptor( executor ex, in_addr ipv4_addr, std::uint16_t const port );
  acceptor( executor ex, in_addr ipv4_addr, std::uint16_t const port,
            int const backlog );

  acceptor( executor ex, in6_addr ipv6_addr, std::uint16_t const port );
  acceptor( executor ex, in6_addr ipv6_addr, std::uint16_t const port,
            int const backlog );

  std::uint16_t port() const noexcept;
  executor get_executor() const noexcept;

  accept_awaitable async_accept();
};

struct stream_close_awaitable;
struct stream_cancel_awaitable;

struct stream {
private:
  friend struct accept_awaitable;

  boost::intrusive_ptr<detail::stream_impl> pstream_;
  stream( executor ex, int fd );

public:
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

struct connect_awaitable;

struct client {
private:
  boost::intrusive_ptr<detail::client_impl> pclient_ = nullptr;

  client( boost::intrusive_ptr<detail::client_impl> pclient );

  void timeout( __kernel_timespec ts );

public:
  client() = delete;
  client( executor ex );

  client( client const& ) = delete;
  client& operator=( client const& ) = delete;

  client( client&& ) = default;
  client& operator=( client&& ) = default;

  connect_awaitable async_connect( in_addr const ipv4_addr,
                                   std::uint16_t const port );
  connect_awaitable async_connect( sockaddr const* addr );

  template <class Rep, class Period>
  void timeout( std::chrono::duration<Rep, Period> const d ) {
    auto ts = detail::duration_to_timespec( d );
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

} // namespace fiona

#endif // FIONA_TCP_HPP