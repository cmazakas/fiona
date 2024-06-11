#ifndef FIONA_IP_HPP
#define FIONA_IP_HPP

#include <cstdint>      // for uint16_t
#include <string>

#include <netinet/in.h> // for sockaddr_in, sockaddr_in6

#include "fiona_export.h"

namespace fiona {
namespace ip {

sockaddr_in FIONA_EXPORT make_sockaddr_ipv4( char const* ipv4_addr,
                                             std::uint16_t const port );

sockaddr_in6 FIONA_EXPORT make_sockaddr_ipv6( char const* ipv6_addr,
                                              std::uint16_t const port );

std::string FIONA_EXPORT to_string( sockaddr_in const* p_ipv4_addr );

} // namespace ip
} // namespace fiona

#endif // FIONA_IP_HPP
