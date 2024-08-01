// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

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

#define USE_TIMEOUTS

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
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

#if defined( USE_TIMEOUTS )
using stream_type = boost::beast::tcp_stream;
#else
using stream_type = boost::asio::ip::tcp::socket;
#endif

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
      []( stream_type stream,
          std::string_view msg ) -> boost::asio::awaitable<void>
  {
    std::size_t num_bytes = 0;

    char buf[128] = {};

    while ( num_bytes < num_msgs * msg.size() ) {

#if defined( USE_TIMEOUTS )
      stream.expires_after( 5s );
#endif

      auto num_read = co_await stream.async_read_some(
          boost::asio::buffer( buf ), boost::asio::deferred );

      auto octets = std::span( buf, num_read );
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      REQUIRE( m == msg );

#if defined( USE_TIMEOUTS )
      stream.expires_after( 5s );
#endif

      auto num_written = co_await stream.async_write_some(
          boost::asio::buffer( m ), boost::asio::deferred );

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
                        std::string_view msg ) -> boost::asio::awaitable<void>
  {
    for ( int i = 0; i < num_clients; ++i ) {
      auto stream = co_await acceptor.async_accept( boost::asio::deferred );

      boost::asio::co_spawn(
          ex, handle_request( stream_type( std::move( stream ) ), msg ),
          boost::asio::detached );
    }

    ++anum_runs;
    co_return;
  };

  auto client = []( boost::asio::any_io_executor ex,
                    std::string_view msg ) -> boost::asio::awaitable<void>
  {
    stream_type client( ex );

#if defined( USE_TIMEOUTS )
    client.expires_after( 5s );
#endif

    co_await client.async_connect( endpoint, boost::asio::deferred );

    std::size_t num_bytes = 0;

    char buf[128] = {};
    while ( num_bytes < num_msgs * msg.size() ) {
#if defined( USE_TIMEOUTS )
      client.expires_after( 5s );
#endif

      auto num_written = co_await client.async_write_some(
          boost::asio::buffer( msg ), boost::asio::deferred );

      REQUIRE( num_written == std::size( msg ) );

#if defined( USE_TIMEOUTS )
      client.expires_after( 5s );
#endif

      auto num_read = co_await client.async_read_some(
          boost::asio::buffer( buf ), boost::asio::deferred );

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
