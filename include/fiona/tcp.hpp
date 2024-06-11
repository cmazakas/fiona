// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_TCP_HPP
#define FIONA_TCP_HPP

#include <fiona/buffer.hpp>                  // for recv_buffer_sequence
#include <fiona/error.hpp>                   // for result
#include <fiona/executor.hpp>                // for executor

#include <fiona/detail/time.hpp>             // for duration_to_timespec

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

#include <chrono>                            // for duration
#include <coroutine>                         // for coroutine_handle
#include <cstddef>                           // for size_t
#include <cstdint>                           // for uint16_t
#include <span>                              // for span
#include <string_view>                       // for string_view

#include "fiona_export.h"

namespace fiona {
namespace tcp {
namespace detail {

struct acceptor_impl;
struct client_impl;
struct stream_impl;

} // namespace detail

struct accept_awaitable;
struct accept_raw_awaitable;
struct connect_awaitable;
struct stream_cancel_awaitable;
struct stream_close_awaitable;
struct send_awaitable;
struct recv_awaitable;

} // namespace tcp
} // namespace fiona

struct __kernel_timespec;
struct sockaddr;
struct sockaddr_in;
struct sockaddr_in6;

namespace fiona {
namespace tcp {
namespace detail {

void FIONA_EXPORT intrusive_ptr_add_ref( acceptor_impl* pacceptor ) noexcept;

void FIONA_EXPORT intrusive_ptr_release( acceptor_impl* pacceptor ) noexcept;

void FIONA_EXPORT intrusive_ptr_add_ref( stream_impl* pstream ) noexcept;

void FIONA_EXPORT intrusive_ptr_release( stream_impl* pstream ) noexcept;

} // namespace detail
} // namespace tcp
} // namespace fiona

namespace fiona {
namespace tcp {

struct acceptor
{
private:
  boost::intrusive_ptr<detail::acceptor_impl> pacceptor_;

public:
  acceptor() = delete;

  acceptor( acceptor const& ) = default;
  acceptor& operator=( acceptor const& ) = default;

  acceptor( acceptor&& ) = default;
  acceptor& operator=( acceptor&& ) = default;

  FIONA_EXPORT
  acceptor( executor ex, sockaddr const* addr );

  acceptor( executor ex, sockaddr_in const* addr )
      : acceptor( ex, reinterpret_cast<sockaddr const*>( addr ) )
  {
  }
  acceptor( executor ex, sockaddr_in6 const* addr )
      : acceptor( ex, reinterpret_cast<sockaddr const*>( addr ) )
  {
  }

  bool operator==( acceptor const& ) const = default;

  FIONA_EXPORT
  std::uint16_t port() const noexcept;

  FIONA_EXPORT
  executor get_executor() const noexcept;

  FIONA_EXPORT
  accept_awaitable async_accept();

  FIONA_EXPORT
  accept_raw_awaitable async_accept_raw();
};

struct FIONA_EXPORT stream
{
protected:
  friend struct accept_awaitable;

  boost::intrusive_ptr<detail::stream_impl> pstream_;

  void timeout( __kernel_timespec ts );
  void cancel_timer();
  void cancel_recv();

public:
  stream() = default;
  stream( executor ex, int fd );

  stream( stream const& ) = default;
  stream& operator=( stream const& ) = default;

  stream( stream&& ) = default;
  stream& operator=( stream&& ) = default;

  virtual ~stream();

  bool operator==( stream const& ) const = default;

  executor get_executor() const noexcept;

  template <class Rep, class Period>
  void
  timeout( std::chrono::duration<Rep, Period> const d )
  {
    auto ts = fiona::detail::duration_to_timespec( d );
    timeout( ts );
  }

  void set_buffer_group( std::uint16_t bgid );

  stream_close_awaitable async_close();
  stream_cancel_awaitable async_cancel();
  send_awaitable async_send( std::string_view msg );
  send_awaitable async_send( std::span<unsigned char const> buf );
  recv_awaitable async_recv();
};

struct FIONA_EXPORT client : public stream
{
public:
  client() {}
  client( executor ex );

  client( client const& ) = default;
  client& operator=( client const& ) = default;

  client( client&& ) = default;
  client& operator=( client&& ) = default;

  virtual ~client() override;

  bool operator==( client const& ) const = default;

  connect_awaitable async_connect( sockaddr const* addr );
  connect_awaitable async_connect( sockaddr_in const* addr );
  connect_awaitable async_connect( sockaddr_in6 const* addr );
};

struct stream_close_awaitable
{
private:
  friend struct stream;
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_;

  stream_close_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  FIONA_EXPORT
  ~stream_close_awaitable();

  bool
  await_ready() const
  {
    return false;
  }

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<void> await_resume();
};

struct stream_cancel_awaitable
{
private:
  friend struct stream;
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_;

  stream_cancel_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  FIONA_EXPORT
  bool await_ready() const;

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<int> await_resume();
};

struct send_awaitable
{
private:
  friend struct stream;
  friend struct client;

  std::span<unsigned char const> buf_;
  boost::intrusive_ptr<detail::stream_impl> pstream_ = nullptr;

  send_awaitable( std::span<unsigned char const> buf,
                  boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  FIONA_EXPORT
  ~send_awaitable();

  bool
  await_ready() const noexcept
  {
    return false;
  }

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<std::size_t> await_resume();
};

struct recv_awaitable
{
private:
  friend struct stream;

  boost::intrusive_ptr<detail::stream_impl> pstream_ = nullptr;

  recv_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  recv_awaitable() = delete;

  recv_awaitable( recv_awaitable const& ) = delete;
  recv_awaitable& operator=( recv_awaitable const& ) = delete;

  recv_awaitable( recv_awaitable&& ) = delete;
  recv_awaitable& operator=( recv_awaitable&& ) = delete;

  FIONA_EXPORT
  ~recv_awaitable();

  FIONA_EXPORT
  bool await_ready() const;

  FIONA_EXPORT
  bool await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<recv_buffer_sequence> await_resume();
};

struct accept_awaitable
{
protected:
  friend struct acceptor;

  boost::intrusive_ptr<detail::acceptor_impl> pacceptor_ = nullptr;

  accept_awaitable( boost::intrusive_ptr<detail::acceptor_impl> pacceptor );

public:
  accept_awaitable() = delete;
  accept_awaitable( accept_awaitable const& ) = delete;
  accept_awaitable( accept_awaitable&& ) = delete;

  FIONA_EXPORT
  ~accept_awaitable();

  bool
  await_ready() const
  {
    return false;
  }

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<stream> await_resume();
};

struct accept_raw_awaitable : public accept_awaitable
{
private:
  friend struct acceptor;

  accept_raw_awaitable( boost::intrusive_ptr<detail::acceptor_impl> pacceptor );

public:
  accept_raw_awaitable() = delete;
  accept_raw_awaitable( accept_awaitable const& ) = delete;
  accept_raw_awaitable( accept_awaitable&& ) = delete;

  FIONA_EXPORT
  result<int> await_resume();
};

struct connect_awaitable
{
private:
  friend struct client;

  boost::intrusive_ptr<detail::stream_impl> pstream_ = nullptr;

  connect_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  FIONA_EXPORT
  ~connect_awaitable();

  FIONA_EXPORT
  bool await_ready() const;

  FIONA_EXPORT
  bool await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<void> await_resume();
};

} // namespace tcp
} // namespace fiona

#endif // FIONA_TCP_HPP
