#include "common.hpp"

#include <boost/config.hpp>

#if defined( BOOST_GCC ) && defined( __SANITIZE_THREAD__ )

#include <iostream>

void
asio_echo_bench()
{
  std::cout << "Asio does not support building with tsan and gcc!" << std::endl;
  CHECK( false );
}

#else

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/beast/core/tcp_stream.hpp>

#include <chrono>
#include <iostream>
#include <span>
#include <thread>

using namespace std::chrono_literals;
using namespace boost::asio::experimental::awaitable_operators;

void
asio_echo_bench()
{
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
      []( boost::beast::tcp_stream stream,
          std::string_view msg ) -> boost::asio::awaitable<void> {
    std::size_t num_bytes = 0;

    char buf[128] = {};

    while ( num_bytes < num_msgs * msg.size() ) {
      stream.expires_after( 5s );
      auto num_read = co_await stream.async_read_some(
          boost::asio::buffer( buf ), boost::asio::use_awaitable );

      auto octets = std::span( buf, num_read );
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      REQUIRE( m == msg );

      stream.expires_after( 5s );
      auto num_written = co_await stream.async_write_some(
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

      boost::asio::co_spawn(
          ex,
          handle_request( boost::beast::tcp_stream( std::move( stream ) ),
                          msg ),
          boost::asio::detached );
    }

    ++anum_runs;
    co_return;
  };

  auto client = []( boost::asio::any_io_executor ex,
                    std::string_view msg ) -> boost::asio::awaitable<void> {
    boost::beast::tcp_stream client( ex );

    client.expires_after( 5s );
    co_await client.async_connect( endpoint, boost::asio::use_awaitable );

    std::size_t num_bytes = 0;

    char buf[128] = {};
    while ( num_bytes < num_msgs * msg.size() ) {
      client.expires_after( 5s );
      auto num_written = co_await client.async_write_some(
          boost::asio::buffer( msg ), boost::asio::use_awaitable );

      REQUIRE( num_written == std::size( msg ) );

      client.expires_after( 5s );
      auto num_read = co_await client.async_read_some(
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
#endif
