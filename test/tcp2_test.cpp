#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/tcpv2.hpp>

static int num_runs = 0;

TEST_CASE( "tcp2_test - acceptor" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  fiona::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  std::cout << "server is listening on port: " << port << std::endl;

  ioc.post( []( fiona::acceptor acceptor ) -> fiona::task<void> {
    std::cout << "starting async_accept() now" << std::endl;
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );
    ++num_runs;
    std::cout << "successfully completed async_accept()" << std::endl;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::client client( ex );
    std::cout << "performing async_connect() now" << std::endl;
    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );
    ++num_runs;
    std::cout << "async_connect() completed" << std::endl;
    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}
