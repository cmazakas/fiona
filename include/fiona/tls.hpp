// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_TLS_HPP
#define FIONA_TLS_HPP

#include <fiona/buffer.hpp>
#include <fiona/executor.hpp>
#include <fiona/task.hpp>
#include <fiona/tcp.hpp>

#include <fiona/detail/config.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace fiona {
namespace tls {

struct FIONA_DECL client : private tcp::client {
public:
  client() = default;
  client( executor ex );

  client( client const& ) = default;
  client& operator=( client const& ) = default;

  client( client&& ) = default;
  client& operator=( client&& ) = default;

  virtual ~client() override;

  bool operator==( client const& ) const = default;

  using tcp::client::async_connect;
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
