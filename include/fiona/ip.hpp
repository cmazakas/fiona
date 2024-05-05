#ifndef FIONA_IP_HPP
#define FIONA_IP_HPP

#include <fiona/detail/config.hpp> // for FIONA_DECL

#include <cstdint>                 // for uint16_t
#include <string>

#include <netinet/in.h>            // for sockaddr_in, sockaddr_in6

namespace fiona {
namespace ip {

sockaddr_in FIONA_DECL
make_sockaddr_ipv4( char const* ipv4_addr, std::uint16_t const port );

sockaddr_in6 FIONA_DECL
make_sockaddr_ipv6( char const* ipv6_addr, std::uint16_t const port );

std::string FIONA_DECL
to_string( sockaddr_in const* p_ipv4_addr );

} // namespace ip
} // namespace fiona

#endif // FIONA_IP_HPP
