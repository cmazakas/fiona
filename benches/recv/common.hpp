#ifndef FIONA_BENCHES_ECHO_COMMON_HPP
#define FIONA_BENCHES_ECHO_COMMON_HPP

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>
#include <catch2/generators/catch_generators_random.hpp>

#include <chrono>

#include <arpa/inet.h>
#include <netinet/in.h>

inline constexpr char const* localhost_ipv4 = "127.0.0.1";

constexpr int num_clients = 1;
constexpr int num_msgs = 1;
constexpr int msg_size = 16 * 1024 * 1024;

void fiona_recv_bench();
void asio_recv_bench();

inline std::vector<std::uint8_t>
make_random_input( std::size_t n )
{
  std::vector<std::uint8_t> v( n, 0 );
  auto rng = Catch::Generators::random( 0, 255 );
  for ( auto& b : v ) {
    b = static_cast<std::uint8_t>( rng.get() );
  }
  return v;
}

#endif // FIONA_BENCHES_ECHO_COMMON_HPP
