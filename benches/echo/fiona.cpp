// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "common.hpp"

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>

#include <iostream>
#include <thread>

CATCH_TRANSLATE_EXCEPTION( fiona::error_code const& ex )
{
  return ex.message();
}

inline constexpr std::size_t const buf_size = 4096;

void
fiona_echo_bench()
{
  static std::atomic_uint64_t anum_runs = 0;
  constexpr std::uint16_t bgid = 0;

  constexpr std::string_view msg = "hello, world!";

  fiona::io_context_params params;
  params.num_files = 16 * 1024;
  params.sq_entries = 256;
  params.cq_entries = 16 * 1024;
  // params.sq_entries = 2 * 4096;
  // params.cq_entries = 2 * 4096;

  fiona::io_context ioc( params );
  auto ex = ioc.get_executor();
  ex.register_buf_ring( 4 * 4096, buf_size, bgid );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ex, &addr );
  auto const port = acceptor.port();

  auto handle_request = []( fiona::executor ex, fiona::tcp::stream stream,
                            std::string_view msg ) -> fiona::task<void>
  {
    // stream.timeout( 5s );

    std::size_t num_bytes = 0;

    stream.set_buffer_group( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto mbufs = co_await stream.async_recv();
      auto bufs = std::move( mbufs ).value();

      auto view = ( *bufs.begin() );

      auto octets = view.readable_bytes();
      auto m = view.as_str();
      {
        auto g = guard();
        REQUIRE( m == msg );
      }

      auto num_written = co_await stream.async_send( octets );

      {
        auto g = guard();
        REQUIRE( num_written.value() == octets.size() );
      }
      num_bytes += octets.size();

      (void)ex;
      // ex.recycle_buffer( bufs.pop_front(), bgid );

      // if ( num_bytes >= ( num_msgs * msg.size() ) / 2 ) {
      //   throw "lmao";
      // }
    }

    co_await stream.async_close();

    ++anum_runs;
  };

  auto server = [handle_request]( fiona::executor ex,
                                  fiona::tcp::acceptor acceptor,
                                  std::string_view msg ) -> fiona::task<void>
  {
    for ( int i = 0; i < num_clients; ++i ) {
      auto stream = co_await acceptor.async_accept();
      ex.spawn( handle_request( ex, std::move( stream.value() ), msg ) );
    }

    ++anum_runs;
    co_return;
  };

  auto client = []( fiona::executor ex, std::uint16_t port,
                    std::string_view msg ) -> fiona::task<void>
  {
    fiona::tcp::client client( ex );
    // client.timeout( 5s );

    auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
    auto mok = co_await client.async_connect( &addr );
    (void)mok;

    std::size_t num_bytes = 0;

    client.set_buffer_group( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto result = co_await client.async_send( msg );
      {
        auto g = guard();
        REQUIRE( result.value() == std::size( msg ) );
      }

      auto mbufs = co_await client.async_recv();
      auto bufs = std::move( mbufs ).value();

      auto view = ( *bufs.begin() );

      auto octets = view.readable_bytes();
      auto m = view.as_str();

      {
        auto g = guard();
        REQUIRE( octets.size() == result.value() );
      }
      {
        auto g = guard();
        REQUIRE( m == msg );
      }

      num_bytes += octets.size();

      // ex.recycle_buffer( bufs.pop_front(), bgid );
    }

    co_await client.async_close();
    ++anum_runs;
  };

  std::thread t1( [&params, &client, port, msg]
  {
    try {
      fiona::io_context ioc( params );
      auto ex = ioc.get_executor();
      ex.register_buf_ring( 4 * 4096, buf_size, bgid );

      for ( int i = 0; i < num_clients; ++i ) {
        ex.spawn( client( ex, port, msg ) );
      }
      ioc.run();

    } catch ( std::exception const& ex ) {
      std::cout << "exception caught in client thread:\n"
                << ex.what() << std::endl;
    } catch ( ... ) {
      std::cout << "unidentified exception caught" << std::endl;
    }
  } );

  ex.spawn( server( ex, std::move( acceptor ), msg ) );
  try {
    ioc.run();
  } catch ( ... ) {
    t1.join();
    throw;
  }

  t1.join();

  {
    auto g = guard();
    REQUIRE( anum_runs == 1 + ( 2 * num_clients ) );
  }
}
