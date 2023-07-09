#include <fiona/io_context.hpp>
#include <fiona/tcp.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE( "accept() tests" ) {
  constexpr std::uint32_t localhost = 0x7f000001;
  constexpr std::uint16_t port = 3030;
  constexpr int num_clients = 1;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ioc.post( ( []( fiona::executor ex ) -> fiona::task<void> {
    fiona::tcp::acceptor acceptor( ex, localhost, port );
    auto a = acceptor.async_accept();

    for ( int num_accepted = 0; num_accepted < num_clients; ++num_accepted ) {
      auto fd = co_await a;
      close( fd.value() );
    }

    co_return;
  } )( ex ) );

  ioc.post( ( []( fiona::executor ex ) -> fiona::task<void> {
    fiona::tcp::client client( ex );

    auto ec = co_await client.async_connect( localhost, port );
    CHECK( !ec );
  } )( ex ) );

  ioc.run();
}
