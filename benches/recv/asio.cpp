#include "common.hpp"

#include <boost/config.hpp>

#if defined( BOOST_GCC ) && defined( __SANITIZE_THREAD__ )

#include <iostream>

void
asio_recv_bench()
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
asio_recv_bench()
{
  static std::atomic_uint64_t anum_runs = 0;

  auto msg = make_random_input( msg_size );

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
          std::span<unsigned char const> msg ) -> boost::asio::awaitable<void>
  {
    std::size_t num_bytes = 0;

    std::vector<unsigned char> body( msg_size );
    unsigned char* p = body.data();

    unsigned char buf[4096] = {};

    while ( num_bytes < num_msgs * msg.size() ) {
      stream.expires_after( 5s );
      auto num_read = co_await stream.async_read_some(
          boost::asio::buffer( buf ), boost::asio::use_awaitable );

      auto octets = std::span( buf, num_read );
      REQUIRE( octets.size() > 0 );

      for ( std::size_t i = 0; i < octets.size(); ++i ) {
        *p++ = octets[i];
      }
      num_bytes += octets.size();

      // REQUIRE( num_written == octets.size() );
    }
    REQUIRE( std::ranges::equal( body, msg ) );

    auto send_buf = msg;
    while ( !send_buf.empty() ) {
      stream.expires_after( 5s );
      auto num_written = co_await stream.async_write_some(
          boost::asio::buffer( send_buf ), boost::asio::use_awaitable );

      send_buf = send_buf.subspan( num_written );
    }

    stream.socket().shutdown( boost::asio::ip::tcp::socket::shutdown_send );
    try {
      co_await stream.async_read_some( boost::asio::buffer( buf ),
                                       boost::asio::use_awaitable );
    } catch ( ... ) {
    }
    stream.socket().close();
    ++anum_runs;
  };

  auto server = [handle_request]( boost::asio::any_io_executor ex,
                                  boost::asio::ip::tcp::acceptor acceptor,
                                  std::span<unsigned char const> msg )
      -> boost::asio::awaitable<void>
  {
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

  auto client =
      []( boost::asio::any_io_executor ex,
          std::span<unsigned char const> msg ) -> boost::asio::awaitable<void>
  {
    boost::beast::tcp_stream client( ex );

    client.expires_after( 5s );
    co_await client.async_connect( endpoint, boost::asio::use_awaitable );

    std::size_t num_bytes = 0;

    std::vector<unsigned char> body( msg_size );
    unsigned char* p = body.data();

    unsigned char buf[4096] = {};

    auto send_buf = msg;
    while ( !send_buf.empty() ) {
      client.expires_after( 5s );
      auto num_written = co_await client.async_write_some(
          boost::asio::buffer( send_buf ), boost::asio::use_awaitable );

      send_buf = send_buf.subspan( num_written );
    }

    while ( num_bytes < num_msgs * msg.size() ) {
      client.expires_after( 5s );
      auto num_read = co_await client.async_read_some(
          boost::asio::buffer( buf ), boost::asio::use_awaitable );

      auto octets = std::span( buf, num_read );
      REQUIRE( octets.size() > 0 );

      for ( std::size_t i = 0; i < octets.size(); ++i ) {
        *p++ = octets[i];
      }
      num_bytes += octets.size();

      // REQUIRE( num_written == octets.size() );
    }
    REQUIRE( std::ranges::equal( body, msg ) );

    client.socket().shutdown( boost::asio::ip::tcp::socket::shutdown_send );
    try {
      co_await client.async_read_some( boost::asio::buffer( buf ),
                                       boost::asio::use_awaitable );
    } catch ( ... ) {
    }
    client.socket().close();

    ++anum_runs;
    co_return;
  };

  std::thread t1( [&client, msg]
  {
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
