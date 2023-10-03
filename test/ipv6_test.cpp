#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

#include <arpa/inet.h>

auto
get_localhost() {
  int ret = -1;
  char const* localhost = "::1";

  in6_addr addr = {};
  ret = inet_pton( AF_INET6, localhost, &addr );
  if ( ret == 0 ) {
    throw "invalid network address was used";
  }

  if ( ret != 1 ) {
    fiona::detail::throw_errno_as_error_code( errno );
  }

  return addr;
}

static int num_runs = 0;

TEST_CASE( "ipv6 sanity check" ) {

  auto server = []( fiona::executor ex,
                    fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    (void)ex;

    auto mstream = co_await acceptor.async_accept();
    auto& stream = mstream.value();

    auto rx = stream.async_recv( 0 );

    auto mbuf = co_await rx;
    auto& buf = mbuf.value();

    auto octets = buf.readable_bytes();
    auto str = buf.as_str();
    CHECK( octets.size() > 0 );
    CHECK( str == "hello, world!" );

    auto n_result = co_await stream.async_write( octets.data(), octets.size() );

    CHECK( n_result.value() == octets.size() );

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex, in6_addr ipv6_addr,
                    std::uint16_t port ) -> fiona::task<void> {
    auto mclient =
        co_await fiona::tcp::client::async_connect( ex, ipv6_addr, port );

    CHECK( mclient.has_value() );

    auto& client = mclient.value();

    co_await fiona::sleep_for( ex, std::chrono::seconds( 1 ) );

    char const msg[] = "hello, world!";
    auto result = co_await client.async_write( msg, std::size( msg ) - 1 );
    CHECK( result.value() == std::size( msg ) - 1 );

    auto rx = client.async_recv( 0 );
    auto mbuf = co_await rx;

    CHECK( mbuf.has_value() );

    auto& buf = mbuf.value();

    auto octets = buf.readable_bytes();
    auto str = buf.as_str();
    CHECK( octets.size() > 0 );
    CHECK( str == "hello, world!" );

    ++num_runs;
    co_return;
  };

  num_runs = 0;

  fiona::io_context ioc;
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto ex = ioc.get_executor();

  auto ipv6_addr = get_localhost();
  fiona::tcp::acceptor acceptor( ex, ipv6_addr, 0 );

  auto port = acceptor.port();

  ioc.post( server( ex, std::move( acceptor ) ) );
  ioc.post( client( ex, ipv6_addr, port ) );

  ioc.run();
  CHECK( num_runs == 2 );
}
