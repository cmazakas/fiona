#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

static in_addr const localhost{ .s_addr = 0x7f000001 };

static int num_runs = 0;

TEST_CASE( "recv timeout" ) {
  num_runs = 0;

  constexpr static auto const client_sleep_dur = std::chrono::seconds( 3 );
  constexpr static auto const server_timeout = std::chrono::seconds( 2 );

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto msession = co_await acceptor.async_accept();
    auto& session = msession.value();
    session.timeout( server_timeout );

    duration_guard dg( client_sleep_dur );
    auto rx = session.async_recv( 0 );

    {
      auto mbuf = co_await rx;
      CHECK( mbuf.has_value() );
    }

    {
      auto mbuf = co_await rx;
      CHECK( mbuf.has_error() );

      auto ec = mbuf.error();
      CHECK( ec == fiona::error_code::from_errno( ECANCELED ) );
    }

    {
      auto mbuf = co_await rx;
      CHECK( mbuf.has_value() );
    }

    ++num_runs;

    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    duration_guard dg( client_sleep_dur );

    auto mclient =
        co_await fiona::tcp::client::async_connect( ex, localhost, port );

    auto& client = mclient.value();

    char const msg[] = { 'r', 'a', 'w', 'r' };
    co_await client.async_write( msg, std::size( msg ) );

    co_await fiona::sleep_for( ex, client_sleep_dur );

    co_await client.async_write( msg, std::size( msg ) );

    ++num_runs;

    co_return;
  };

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.register_buffer_sequence( 1024, 128, 0 );

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );

  auto port = acceptor.port();

  ex.post( server( std::move( acceptor ) ) );
  ex.post( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "recv high traffic" ) {
  num_runs = 0;

  auto server = []( fiona::tcp::acceptor acceptor,
                    fiona::executor ex ) -> fiona::task<void> {
    auto msession = co_await acceptor.async_accept();
    auto& session = msession.value();
    session.timeout( std::chrono::seconds( 1 ) );

    auto rx = session.async_recv( 0 );

    for ( int i = 0; i < 4; ++i ) {
      auto mbuf = co_await rx;
      CHECK( mbuf.has_value() );
      co_await fiona::sleep_for( ex, std::chrono::milliseconds( 500 ) );
    }

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    auto mclient =
        co_await fiona::tcp::client::async_connect( ex, localhost, port );

    auto& client = mclient.value();

    char const msg[] = { 'r', 'a', 'w', 'r' };

    for ( int i = 0; i < 4; ++i ) {
      co_await client.async_write( msg, std::size( msg ) );
      co_await fiona::sleep_for( ex, std::chrono::milliseconds( 500 ) );
    }

    ++num_runs;
    co_return;
  };

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.register_buffer_sequence( 1024, 128, 0 );

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );

  auto port = acceptor.port();

  ex.post( server( std::move( acceptor ), ex ) );
  ex.post( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}
