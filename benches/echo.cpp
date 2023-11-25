#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/tcp.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <array>
#include <chrono>
#include <random>
#include <span>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>

using namespace std::chrono_literals;

static in_addr const localhost_ipv4 = { .s_addr = htonl( 0x7f000001 ) };

constexpr int num_clients = 5000;
constexpr int num_msgs = 10;

CATCH_TRANSLATE_EXCEPTION( fiona::error_code const& ex ) {
  return ex.message();
}

TEST_CASE( "asio echo bench" ) {
  static std::atomic_uint64_t anum_runs = 0;

  constexpr std::string_view msg = "hello, world!";

  boost::asio::io_context ioc( 1 );
  auto ex = ioc.get_executor();

  boost::asio::ip::tcp::acceptor acceptor( ioc );
  static boost::asio::ip::tcp::endpoint endpoint(
      boost::asio::ip::address_v4( { 127, 0, 0, 1 } ), 3301 );
  acceptor.open( endpoint.protocol() );
  acceptor.set_option( boost::asio::ip::tcp::acceptor::reuse_address( true ) );
  acceptor.bind( endpoint );
  acceptor.listen();

  auto handle_request =
      []( boost::asio::ip::tcp::socket stream,
          std::string_view msg ) -> boost::asio::awaitable<void> {
    std::size_t num_bytes = 0;

    char buf[128] = {};

    while ( num_bytes < num_msgs * msg.size() ) {
      auto num_read = co_await stream.async_receive(
          boost::asio::buffer( buf ), boost::asio::use_awaitable );

      auto octets = std::span( buf, num_read );
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      REQUIRE( m == msg );

      auto num_written = co_await stream.async_send(
          boost::asio::buffer( m ), boost::asio::use_awaitable );

      REQUIRE( num_written == octets.size() );
      num_bytes += octets.size();

      // if ( num_bytes >= ( num_msgs * msg.size() ) / 2 ) {
      //   throw "lmao";
      // }
    }

    ++anum_runs;
  };

  auto server =
      [handle_request]( boost::asio::any_io_executor ex,
                        boost::asio::ip::tcp::acceptor acceptor,
                        std::string_view msg ) -> boost::asio::awaitable<void> {
    for ( int i = 0; i < num_clients; ++i ) {
      auto stream =
          co_await acceptor.async_accept( boost::asio::use_awaitable );

      boost::asio::co_spawn( ex, handle_request( std::move( stream ), msg ),
                             boost::asio::detached );
    }

    ++anum_runs;
    co_return;
  };

  auto client = []( boost::asio::any_io_executor ex,
                    std::string_view msg ) -> boost::asio::awaitable<void> {
    boost::asio::ip::tcp::socket client( ex );

    co_await client.async_connect( endpoint, boost::asio::use_awaitable );

    std::size_t num_bytes = 0;

    char buf[128] = {};
    while ( num_bytes < num_msgs * msg.size() ) {
      auto num_written = co_await client.async_send(
          boost::asio::buffer( msg ), boost::asio::use_awaitable );

      REQUIRE( num_written == std::size( msg ) );

      auto num_read = co_await client.async_receive(
          boost::asio::buffer( buf ), boost::asio::use_awaitable );

      REQUIRE( num_written == num_read );

      auto octets = std::span( buf, num_read );
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      REQUIRE( m == msg );

      num_bytes += octets.size();
    }

    ++anum_runs;
    co_return;
  };

  std::thread t1( [&client, msg] {
    try {
      boost::asio::io_context ioc( 1 );

      auto ex = ioc.get_executor();
      for ( int i = 0; i < num_clients; ++i ) {
        boost::asio::co_spawn( ex, client( ex, msg ), boost::asio::detached );
      }
      REQUIRE( ioc.run() > 0 );

    } catch ( std::exception const& ex ) {
      std::cout << "exception caught in client thread:\n"
                << ex.what() << std::endl;
    }
  } );

  boost::asio::co_spawn( ex, server( ex, std::move( acceptor ), msg ),
                         boost::asio::detached );

  try {
    REQUIRE( ioc.run() > 0 );
  } catch ( ... ) {
    t1.join();
    throw;
  }

  t1.join();

  REQUIRE( anum_runs == 1 + ( 2 * num_clients ) );
}

TEST_CASE( "fiona tcp echo" ) {
  static std::atomic_uint64_t anum_runs = 0;
  constexpr std::uint16_t bgid = 0;

  constexpr std::string_view msg = "hello, world!";

  fiona::io_context_params params;
  params.num_files = 4 * 4096;
  params.sq_entries = 4 * 4096;
  params.cq_entries = 8 * 4096;

  fiona::io_context ioc( params );
  ioc.register_buffer_sequence( 4 * 4096, 128, bgid );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();

  auto handle_request = []( fiona::executor, fiona::tcp::stream stream,
                            std::string_view msg ) -> fiona::task<void> {
    stream.timeout( 5s );

    std::size_t num_bytes = 0;

    auto rx = stream.get_receiver( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto borrowed_buf = co_await rx.async_recv();
      REQUIRE( borrowed_buf.has_value() );

      auto octets = borrowed_buf.value().readable_bytes();
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      REQUIRE( m == msg );

      auto num_written = co_await stream.async_send( octets );

      REQUIRE( !num_written.has_error() );
      REQUIRE( static_cast<std::size_t>( num_written.value() ) ==
               octets.size() );
      num_bytes += octets.size();

      // if ( num_bytes >= ( num_msgs * msg.size() ) / 2 ) {
      //   throw "lmao";
      // }
    }

    ++anum_runs;
  };

  auto server = [handle_request]( fiona::executor ex,
                                  fiona::tcp::acceptor acceptor,
                                  std::string_view msg ) -> fiona::task<void> {
    for ( int i = 0; i < num_clients; ++i ) {
      auto stream = co_await acceptor.async_accept();
      ex.post( handle_request( ex, std::move( stream.value() ), msg ) );
    }

    ++anum_runs;
    co_return;
  };

  auto client = []( fiona::executor ex, std::uint16_t port,
                    std::string_view msg ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    client.timeout( 5s );

    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    REQUIRE( mok.has_value() );

    std::size_t num_bytes = 0;

    auto rx = client.get_receiver( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto result = co_await client.async_send( msg );
      REQUIRE( result.value() == std::size( msg ) );

      auto borrowed_buf = co_await rx.async_recv();

      auto octets = borrowed_buf.value().readable_bytes();
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      REQUIRE( m == msg );

      num_bytes += octets.size();
    }

    ++anum_runs;
  };

  std::thread t1( [&params, &client, port, msg] {
    try {
      fiona::io_context ioc( params );
      ioc.register_buffer_sequence( 4 * 4096, 128, bgid );

      auto ex = ioc.get_executor();
      for ( int i = 0; i < num_clients; ++i ) {
        ioc.post( client( ex, port, msg ) );
      }
      ioc.run();

    } catch ( std::exception const& ex ) {
      std::cout << "exception caught in client thread:\n"
                << ex.what() << std::endl;
    }
  } );

  ioc.post( server( ex, std::move( acceptor ), msg ) );
  try {
    ioc.run();
  } catch ( ... ) {
    t1.join();
    throw;
  }

  t1.join();

  REQUIRE( anum_runs == 1 + ( 2 * num_clients ) );
}
