#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/tcpv2.hpp>
#include <fiona/time.hpp>

static int num_runs = 0;

TEST_CASE( "tcp2_test - acceptor" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  fiona::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  ioc.post( []( fiona::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );
    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::client client( ex );
    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );
    ++num_runs;
    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "tcp2_test - client already connected" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  fiona::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  ioc.post( []( fiona::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );

    fiona::timer timer( acceptor.get_executor() );
    co_await timer.async_wait( std::chrono::milliseconds( 500 ) );

    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::client client( ex );
    {
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      CHECK( mok.has_value() );
    }

    auto timer = fiona::timer( ex );
    co_await timer.async_wait( std::chrono::milliseconds( 100 ) );

    {
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      CHECK( mok.has_error() );
    }

    {
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      CHECK( mok.has_error() );
    }

    ++num_runs;

    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}
