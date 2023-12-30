#ifndef FIONA_IP_HPP
#define FIONA_IP_HPP

#include <cstdint>

#include <netinet/in.h>
#include <sys/socket.h>

namespace fiona {
namespace ip {

sockaddr_in
make_sockaddr_ipv4( char const* ipv4_addr, std::uint16_t const port );

sockaddr_in6
make_sockaddr_ipv6( char const* ipv6_addr, std::uint16_t const port );

} // namespace ip
} // namespace fiona

#endif // FIONA_IP_HPP
