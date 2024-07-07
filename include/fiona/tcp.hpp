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
namespace tls {

class client;
class server;

} // namespace tls
} // namespace fiona

namespace fiona {
namespace tcp {
namespace detail {

struct acceptor_impl;
struct client_impl;
struct stream_impl;

} // namespace detail

class accept_awaitable;
class accept_raw_awaitable;
class connect_awaitable;
class stream_cancel_awaitable;
class stream_close_awaitable;
class send_awaitable;
class recv_awaitable;
class shutdown_awaitable;

} // namespace tcp
} // namespace fiona

struct __kernel_timespec;
struct sockaddr;
struct sockaddr_in;
struct sockaddr_in6;

namespace fiona {
namespace tcp {
namespace detail {

void FIONA_EXPORT intrusive_ptr_add_ref( acceptor_impl* p_acceptor ) noexcept;
void FIONA_EXPORT intrusive_ptr_release( acceptor_impl* p_acceptor ) noexcept;

void FIONA_EXPORT intrusive_ptr_add_ref( stream_impl* p_stream ) noexcept;
void FIONA_EXPORT intrusive_ptr_release( stream_impl* p_stream ) noexcept;

} // namespace detail
} // namespace tcp
} // namespace fiona

namespace fiona {
namespace tcp {

class acceptor
{
  boost::intrusive_ptr<detail::acceptor_impl> p_acceptor_;

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

//------------------------------------------------------------------------------

class FIONA_EXPORT stream
{
  friend class client;
  friend class tls::client;
  friend class tls::server;
  friend class accept_awaitable;

  boost::intrusive_ptr<detail::stream_impl> p_stream_;

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
  shutdown_awaitable async_shutdown( int how );
};

//------------------------------------------------------------------------------

class FIONA_EXPORT client : public stream
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

//------------------------------------------------------------------------------

class stream_close_awaitable
{
  friend class stream;
  friend class client;

  boost::intrusive_ptr<detail::stream_impl> p_stream_;

  stream_close_awaitable( boost::intrusive_ptr<detail::stream_impl> p_stream );

public:
  stream_close_awaitable() = delete;

  stream_close_awaitable( stream_close_awaitable const& ) = delete;
  stream_close_awaitable& operator=( stream_close_awaitable const& ) = delete;

  FIONA_EXPORT
  ~stream_close_awaitable();

  bool
  await_ready() const noexcept
  {
    return false;
  }

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<void> await_resume();
};

//------------------------------------------------------------------------------

class stream_cancel_awaitable
{
  friend class stream;
  friend class client;

  boost::intrusive_ptr<detail::stream_impl> p_stream_;

  stream_cancel_awaitable( boost::intrusive_ptr<detail::stream_impl> p_stream );

public:
  stream_cancel_awaitable() = delete;

  stream_cancel_awaitable( stream_cancel_awaitable const& ) = delete;
  stream_cancel_awaitable& operator=( stream_cancel_awaitable const& ) = delete;

  bool
  await_ready() const noexcept
  {
    return false;
  }

  FIONA_EXPORT
  void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<int> await_resume();
};

//------------------------------------------------------------------------------

class send_awaitable
{
  friend class stream;
  friend class client;

  std::span<unsigned char const> buf_;
  boost::intrusive_ptr<detail::stream_impl> p_stream_ = nullptr;

  send_awaitable( std::span<unsigned char const> buf,
                  boost::intrusive_ptr<detail::stream_impl> p_stream );

public:
  send_awaitable() = delete;

  send_awaitable( send_awaitable const& ) = delete;
  send_awaitable& operator=( send_awaitable const& ) = delete;

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

//------------------------------------------------------------------------------

class recv_awaitable
{
  friend class stream;

  boost::intrusive_ptr<detail::stream_impl> p_stream_ = nullptr;

  recv_awaitable( boost::intrusive_ptr<detail::stream_impl> p_stream );

public:
  recv_awaitable() = delete;

  recv_awaitable( recv_awaitable const& ) = delete;
  recv_awaitable& operator=( recv_awaitable const& ) = delete;

  FIONA_EXPORT
  ~recv_awaitable();

  FIONA_EXPORT
  bool await_ready() const;

  FIONA_EXPORT
  bool await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT
  result<recv_buffer_sequence> await_resume();
};

//------------------------------------------------------------------------------

class shutdown_awaitable
{
  friend class stream;

  boost::intrusive_ptr<detail::stream_impl> p_stream_ = nullptr;

  shutdown_awaitable( boost::intrusive_ptr<detail::stream_impl> p_stream );

public:
  shutdown_awaitable() = delete;

  shutdown_awaitable( shutdown_awaitable const& ) = delete;
  shutdown_awaitable& operator=( shutdown_awaitable const& ) = delete;

  FIONA_EXPORT
  ~shutdown_awaitable();

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

//------------------------------------------------------------------------------

class accept_awaitable
{
private:
  friend class acceptor;
  friend class accept_raw_awaitable;

  boost::intrusive_ptr<detail::acceptor_impl> p_acceptor_ = nullptr;

  accept_awaitable( boost::intrusive_ptr<detail::acceptor_impl> p_acceptor );

public:
  accept_awaitable() = delete;

  accept_awaitable( accept_awaitable const& ) = delete;
  accept_awaitable& operator=( accept_awaitable const& ) = delete;

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

//------------------------------------------------------------------------------

class accept_raw_awaitable : public accept_awaitable
{
private:
  friend class acceptor;

  accept_raw_awaitable(
      boost::intrusive_ptr<detail::acceptor_impl> p_acceptor );

public:
  accept_raw_awaitable() = delete;

  accept_raw_awaitable( accept_awaitable const& ) = delete;
  accept_raw_awaitable& operator=( accept_raw_awaitable const& ) = delete;

  FIONA_EXPORT
  result<int> await_resume();
};

//------------------------------------------------------------------------------

class connect_awaitable
{
  friend class client;

  boost::intrusive_ptr<detail::stream_impl> pstream_ = nullptr;

  connect_awaitable( boost::intrusive_ptr<detail::stream_impl> pstream );

public:
  connect_awaitable() = delete;

  connect_awaitable( connect_awaitable const& ) = delete;
  connect_awaitable& operator=( connect_awaitable const& ) = delete;

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
