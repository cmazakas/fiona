#include <fiona/ip.hpp>

#include <fiona/error.hpp>         // for throw_errno_as_error_code

#include <fiona/detail/config.hpp> // for FIONA_DECL

#include <arpa/inet.h>             // for htons, inet_pton
#include <errno.h>                 // for errno
#include <sys/socket.h>            // for AF_INET, AF_INET6

namespace fiona {
namespace ip {

sockaddr_in FIONA_DECL
make_sockaddr_ipv4( char const* ipv4_addr, std::uint16_t const port ) {
  int ret = -1;

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons( port );

  ret = inet_pton( AF_INET, ipv4_addr, &addr.sin_addr );

  if ( ret == 0 ) {
    throw "invalid network address was used";
  }

  if ( ret != 1 ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  return addr;
}

sockaddr_in6 FIONA_DECL
make_sockaddr_ipv6( char const* ipv6_addr, std::uint16_t const port ) {
  int ret = -1;

  sockaddr_in6 addr = {};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons( port );

  ret = inet_pton( AF_INET6, ipv6_addr, &addr.sin6_addr );

  if ( ret == 0 ) {
    throw "invalid network address was used";
  }

  if ( ret != 1 ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  return addr;
}

} // namespace ip
} // namespace fiona
