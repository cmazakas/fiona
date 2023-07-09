#include <fiona/io_context.hpp>
#include <fiona/sleep.hpp>
#include <fiona/tcp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>

constexpr std::uint32_t localhost = 0x7f000001;

std::uint16_t
get_port() {
  static std::atomic_uint16_t port = 3030;
  return ++port;
}

static int num_runs = 0;

TEST_CASE( "accept sanity test" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  auto const port = get_port();

  ioc.post(
      ( []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
        fiona::tcp::acceptor acceptor( ex, localhost, port );
        auto a = acceptor.async_accept();

        auto fd = co_await a;
        close( fd.value() );

        ++num_runs;
        co_return;
      } )( ex, port ) );

  ioc.post(
      ( []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
        fiona::tcp::client client( ex );

        auto ec = co_await client.async_connect( localhost, port );
        CHECK( !ec );
        ++num_runs;
      } )( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "accept back-pressure test" ) {
  // this test purposefully doesn't exceed the size of the completion queue so
  // that our multishot accept() doesn't need to be rescheduled

  constexpr std::size_t num_clients = 100;
  REQUIRE( num_clients < fiona::io_context::cq_entries );

  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  auto const port = get_port();

  ex.post( ( []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
    fiona::tcp::acceptor acceptor( ex, localhost, port );
    auto a = acceptor.async_accept();

    {
      // actually start the multishot accept sqe
      auto fd = co_await a;
      close( fd.value() );
    }

    co_await fiona::sleep_for( ex, std::chrono::milliseconds( 200 ) );

    for ( unsigned i = 1; i < num_clients; ++i ) {
      // ideally, this doesn't suspend at all and instead we just start pulling
      // from the queue of waiting connections
      auto fd = co_await a;
      CHECK( fd.value() >= 0 );
      close( fd.value() );
    }

    ++num_runs;
  } )( ex, port ) );

  for ( unsigned i = 0; i < num_clients; ++i ) {
    ex.post(
        ( []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
          fiona::tcp::client client( ex );

          auto ec = co_await client.async_connect( localhost, port );
          CHECK( !ec );
          ++num_runs;
        } )( ex, port ) );
  }

  ioc.run();

  CHECK( num_runs == num_clients + 1 );
}

TEST_CASE( "accept CQ overflow" ) {
  // this test purposefully exceeds the size of the completion queue so
  // that our multishot accept() needs to be rescheduled

  constexpr std::size_t num_clients = 5000;
  REQUIRE( num_clients > fiona::io_context::cq_entries );

  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  auto const port = get_port();

  ex.post( ( []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
    fiona::tcp::acceptor acceptor( ex, localhost, port );
    auto a = acceptor.async_accept();

    {
      auto fd = co_await a;
      close( fd.value() );
    }

    co_await fiona::sleep_for( ex, std::chrono::milliseconds( 200 ) );

    for ( unsigned i = 1; i < num_clients; ++i ) {
      auto fd = co_await a;
      close( fd.value() );
    }

    ++num_runs;
  } )( ex, port ) );

  for ( unsigned i = 0; i < num_clients; ++i ) {
    ex.post(
        ( []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
          fiona::tcp::client client( ex );

          auto ec = co_await client.async_connect( localhost, port );
          CHECK( !ec );
          ++num_runs;
        } )( ex, port ) );
  }

  ioc.run();

  CHECK( num_runs == num_clients + 1 );
}
