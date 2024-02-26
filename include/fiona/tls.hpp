#ifndef FIONA_TLS_HPP
#define FIONA_TLS_HPP

#include <fiona/executor.hpp>
#include <fiona/task.hpp>
#include <fiona/tcp.hpp>

#include <fiona/detail/config.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp> // for intrusive_ptr

namespace fiona {
namespace tls {

struct connect_awaitable;

namespace detail {

struct client_impl;

void FIONA_DECL
intrusive_ptr_add_ref( client_impl* pclient ) noexcept;

void FIONA_DECL
intrusive_ptr_release( client_impl* pclient ) noexcept;

} // namespace detail

struct client {
private:
  boost::intrusive_ptr<detail::client_impl> pclient_;

public:
  client() = default;

  FIONA_DECL
  client( executor ex );

  client( client const& ) = default;
  client& operator=( client const& ) = default;

  client( client&& ) = default;
  client& operator=( client&& ) = default;

  ~client() = default;

  bool operator==( client const& ) const = default;

  FIONA_DECL
  tcp::client as_tcp() const noexcept;

  FIONA_DECL
  task<void> async_handshake();
};

} // namespace tls
} // namespace fiona

#endif // FIONA_TLS_HPP
