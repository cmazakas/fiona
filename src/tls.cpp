#include <fiona/dns.hpp>
#include <fiona/tls.hpp>

#include "fiona/executor.hpp"
#include "stream_impl.hpp"

namespace fiona {
namespace tls {
namespace detail {
struct client_impl : public tcp::detail::client_impl {
  client_impl() = delete;
  client_impl( client_impl const& ) = delete;
  client_impl( client_impl&& ) = delete;

  client_impl( executor ex ) : tcp::detail::client_impl( ex ) {}
};

void FIONA_DECL
intrusive_ptr_add_ref( client_impl* pclient ) noexcept {
  ++pclient->count_;
}

void FIONA_DECL
intrusive_ptr_release( client_impl* pclient ) noexcept {
  --pclient->count_;
  if ( pclient->count_ == 0 ) {
    delete pclient;
  }
}

} // namespace detail

client::client( executor ex ) : pclient_{ new detail::client_impl{ ex } } {}

tcp::client
client::as_tcp() const noexcept {
  return { pclient_.get() };
}

} // namespace tls
} // namespace fiona
