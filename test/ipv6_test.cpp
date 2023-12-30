#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

static int num_runs = 0;

TEST_CASE( "ipv6 sanity check" ) {

  auto server = []( fiona::executor ex,
                    fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    (void)ex;

    auto mstream = co_await acceptor.async_accept();
    auto& stream = mstream.value();

    auto rx = stream.get_receiver( 0 );

    auto mbuf = co_await rx.async_recv();
    auto& buf = mbuf.value();

    auto octets = buf.readable_bytes();
    auto str = buf.as_str();
    CHECK( octets.size() > 0 );
    CHECK( str == "hello, world!" );

    auto n_result = co_await stream.async_send( octets );

    CHECK( n_result.value() == octets.size() );

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex,
                    sockaddr_in6 ipv6_addr ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    fiona::timer timer( ex );

    auto mok = co_await client.async_connect( &ipv6_addr );

    CHECK( mok.has_value() );

    co_await timer.async_wait( 1s );

    auto const msg = std::string_view( "hello, world!" );
    auto result = co_await client.async_send( msg );
    CHECK( result.value() == msg.size() );

    auto rx = client.get_receiver( 0 );

    auto mbuf = co_await rx.async_recv();

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

  auto server_addr = fiona::ip::make_sockaddr_ipv6( localhost_ipv6, 0 );
  fiona::tcp::acceptor acceptor( ex, &server_addr );

  auto port = acceptor.port();

  server_addr = fiona::ip::make_sockaddr_ipv6( localhost_ipv6, port );
  ioc.post( server( ex, std::move( acceptor ) ) );
  ioc.post( client( ex, server_addr ) );

  ioc.run();
  CHECK( num_runs == 2 );
}
