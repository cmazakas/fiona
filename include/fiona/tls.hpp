// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_TLS_HPP
#define FIONA_TLS_HPP

#include <fiona/buffer.hpp>
#include <fiona/executor.hpp>
#include <fiona/task.hpp>
#include <fiona/tcp.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <memory>

#include "fiona_tls_export.h"

namespace fiona {
namespace tls {

struct tls_context;

namespace detail {

struct client_impl;
struct server_impl;
struct tls_context_frame;

} // namespace detail

//------------------------------------------------------------------------------

struct tls_context
{
private:
  friend struct detail::client_impl;
  friend struct detail::server_impl;

  std::shared_ptr<detail::tls_context_frame> p_tls_frame_;

public:
  FIONA_TLS_EXPORT
  tls_context();

  ~tls_context() = default;

  tls_context( tls_context const& ) = default;
  tls_context& operator=( tls_context const& ) = default;

  FIONA_TLS_EXPORT
  void add_certificate_authority( std::string_view filepath );

  FIONA_TLS_EXPORT
  void add_certificate_key_pair( std::string_view cert_path,
                                 std::string_view key_path );
};

//------------------------------------------------------------------------------

class FIONA_TLS_EXPORT client : private tcp::client
{
public:
  client() = default;
  client( tls_context ctx, executor ex, std::string_view hostname );

  client( client const& ) = default;
  client& operator=( client const& ) = default;

  client( client&& ) = default;
  client& operator=( client&& ) = default;

  virtual ~client() override;

  bool operator==( client const& ) const = default;

  using tcp::client::async_connect;
  using tcp::stream::async_close;
  using tcp::stream::set_buffer_group;

  task<result<void>> async_handshake();
  task<result<std::size_t>> async_send( std::span<unsigned char const> buf );
  task<result<std::size_t>> async_send( std::string_view msg );
  task<result<recv_buffer_sequence>> async_recv();
  task<result<void>> async_shutdown();
};

class FIONA_TLS_EXPORT server : private tcp::stream
{
public:
  server() = default;
  server( tls_context ctx, executor ex, int fd );

  server( server const& ) = default;
  server& operator=( server const& ) = default;

  server( server&& ) = default;
  server& operator=( server&& ) = default;

  virtual ~server() override;

  bool operator==( server const& ) const = default;

  using tcp::stream::async_close;
  using tcp::stream::get_executor;
  using tcp::stream::set_buffer_group;

  task<result<void>> async_handshake();
  task<result<std::size_t>> async_send( std::span<unsigned char const> buf );
  task<result<std::size_t>> async_send( std::string_view msg );
  task<result<recv_buffer_sequence>> async_recv();
  task<result<void>> async_shutdown();
};

} // namespace tls
} // namespace fiona

#endif // FIONA_TLS_HPP
