#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

static int num_runs = 0;

TEST_CASE( "ipv6 sanity check" )
{

  auto server = []( fiona::executor ex,
                    fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    (void)ex;

    auto mstream = co_await acceptor.async_accept();
    auto& stream = mstream.value();

    stream.set_buffer_group( 0 );

    auto mbufs = co_await stream.async_recv();
    auto bufs = std::move( mbufs ).value();

    auto octets = bufs.to_bytes();
    auto str = bufs.to_string();
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

    client.set_buffer_group( 0 );

    auto mbufs = co_await client.async_recv();
    auto bufs = std::move( mbufs ).value();

    auto octets = bufs.to_bytes();
    auto str = bufs.to_string();
    CHECK( octets.size() > 0 );
    CHECK( str == "hello, world!" );

    ++num_runs;
    co_return;
  };

  num_runs = 0;

  fiona::io_context ioc;

  auto ex = ioc.get_executor();
  ex.register_buffer_sequence( 1024, 128, 0 );

  auto server_addr = fiona::ip::make_sockaddr_ipv6( localhost_ipv6, 0 );
  fiona::tcp::acceptor acceptor( ex, &server_addr );

  auto port = acceptor.port();

  server_addr = fiona::ip::make_sockaddr_ipv6( localhost_ipv6, port );
  ex.spawn( server( ex, std::move( acceptor ) ) );
  ex.spawn( client( ex, server_addr ) );

  ioc.run();
  CHECK( num_runs == 2 );
}
