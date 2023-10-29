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
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );
    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.run();

  CHECK( num_runs == 1 );
}
