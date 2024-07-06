// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "common.hpp"

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

#include <iostream>
#include <thread>

using namespace std::chrono_literals;

CATCH_TRANSLATE_EXCEPTION( fiona::error_code const& ex )
{
  return ex.message();
}

inline constexpr std::size_t const buf_size = 4096;
inline constexpr std::size_t const num_bufs = 1024;

void
fiona_recv_bench()
{
  static std::atomic_uint64_t anum_runs = 0;
  constexpr std::uint16_t server_bgid = 0;
  constexpr std::uint16_t client_bgid = 1;

  auto msg = make_random_input( msg_size );

  fiona::io_context_params params;
  params.num_files = 16 * 1024;
  params.sq_entries = 256;
  params.cq_entries = 16 * 1024;
  // params.sq_entries = 2 * 4096;
  // params.cq_entries = 2 * 4096;

  fiona::io_context ioc( params );
  auto ex = ioc.get_executor();
  ex.register_buffer_sequence( num_bufs, buf_size, server_bgid );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ex, &addr );
  auto const port = acceptor.port();

  auto handle_request =
      []( fiona::executor, fiona::tcp::stream stream,
          std::span<unsigned char const> msg ) -> fiona::task<void>
  {
    stream.timeout( 5s );

    std::size_t num_bytes = 0;

    stream.set_buffer_group( server_bgid );

    std::vector<unsigned char> body( msg_size );
    unsigned char* p = body.data();

    while ( num_bytes < num_msgs * msg.size() ) {
      auto mbufs = co_await stream.async_recv();
      if ( mbufs.has_error() &&
           mbufs.error() == fiona::error_code::from_errno( ENOBUFS ) ) {
        continue;
      }

      auto bufs = std::move( mbufs ).value();
      for ( auto view : bufs ) {
        auto octets = view.readable_bytes();
        REQUIRE( octets.size() > 0 );

        for ( std::size_t i = 0; i < octets.size(); ++i ) {
          *p++ = octets[i];
        }

        num_bytes += octets.size();
      }
    }
    REQUIRE( std::ranges::equal( body, msg ) );

    auto send_buf = msg;
    while ( !send_buf.empty() ) {
      auto m_sent = co_await stream.async_send( send_buf );
      if ( m_sent.has_error() ) {
        CHECK( m_sent.error() == fiona::error_code::from_errno( ETIMEDOUT ) );
      }
      REQUIRE( m_sent.has_value() );
      auto sent = *m_sent;
      send_buf = send_buf.subspan( sent );
    }

    co_await stream.async_shutdown( SHUT_WR );
    co_await stream.async_recv();
    co_await stream.async_close();
    ++anum_runs;
  };

  auto server = [handle_request](
                    fiona::executor ex, fiona::tcp::acceptor acceptor,
                    std::span<unsigned char const> msg ) -> fiona::task<void>
  {
    for ( int i = 0; i < num_clients; ++i ) {
      auto stream = co_await acceptor.async_accept();
      ex.spawn( handle_request( ex, std::move( stream.value() ), msg ) );
    }

    ++anum_runs;
    co_return;
  };

  auto client = []( fiona::executor ex, std::uint16_t port,
                    std::span<unsigned char const> msg ) -> fiona::task<void>
  {
    fiona::tcp::client client( ex );
    client.timeout( 5s );

    auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
    auto mok = co_await client.async_connect( &addr );
    (void)mok;

    std::size_t num_bytes = 0;

    client.set_buffer_group( client_bgid );

    std::vector<unsigned char> body( msg_size );
    unsigned char* p = body.data();

    auto send_buf = msg;
    while ( !send_buf.empty() ) {
      auto m_sent = co_await client.async_send( send_buf );
      REQUIRE( m_sent.has_value() );
      auto sent = *m_sent;
      send_buf = send_buf.subspan( sent );
    }

    while ( num_bytes < num_msgs * msg.size() ) {
      auto mbufs = co_await client.async_recv();
      if ( mbufs.has_error() &&
           mbufs.error() == fiona::error_code::from_errno( ENOBUFS ) ) {
        continue;
      }

      auto bufs = std::move( mbufs ).value();
      for ( auto view : bufs ) {
        auto octets = view.readable_bytes();
        if ( octets.size() == 0 ) {
          REQUIRE( num_bytes == num_msgs * msg.size() );
        }

        for ( std::size_t i = 0; i < octets.size(); ++i ) {
          *p++ = octets[i];
        }

        num_bytes += octets.size();
      }
    }
    REQUIRE( std::ranges::equal( body, msg ) );

    co_await client.async_shutdown( SHUT_WR );
    co_await client.async_recv();
    co_await client.async_close();
    ++anum_runs;
  };

  std::thread t1( [&params, &client, port, msg]
  {
    try {
      fiona::io_context ioc( params );
      auto ex = ioc.get_executor();
      ex.register_buffer_sequence( num_bufs, buf_size, client_bgid );

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

  REQUIRE( anum_runs == 1 + ( 2 * num_clients ) );
}
