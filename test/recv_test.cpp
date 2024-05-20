// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "helpers.hpp"

#include <fiona/detail/common.hpp>
#include <fiona/error.hpp>
#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

#include <algorithm>
#include <cerrno>

static int num_runs = 0;

TEST_CASE( "recv timeout" )
{
  num_runs = 0;

  constexpr static auto const server_timeout = 1s;
  constexpr static auto const client_sleep_dur = 2s;

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto msession = co_await acceptor.async_accept();
    auto& session = msession.value();
    session.timeout( server_timeout );
    session.timeout( server_timeout );
    session.timeout( server_timeout );
    session.timeout( server_timeout );

    std::uint16_t buffer_group_id = 0;
    session.set_buffer_group( buffer_group_id );

    {
      auto mbuf = co_await session.async_recv();
      CHECK( mbuf.has_value() );
    }

    {
      auto mbuf = co_await session.async_recv();
      CHECK( mbuf.has_error() );
      CHECK( mbuf.error() == fiona::error_code::from_errno( ETIMEDOUT ) );
    }

    fiona::timer timer( session.get_executor() );
    co_await timer.async_wait( server_timeout );

    {
      auto mbuf = co_await session.async_recv();
      CHECK( mbuf.has_value() );
    }

    ++num_runs;

    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );

    auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
    auto mok = co_await client.async_connect( &addr );
    CHECK( mok.has_value() );

    auto sv = std::string_view( "rawr" );

    auto mbytes_transferred = co_await client.async_send( sv );
    CHECK( mbytes_transferred.value() == sv.size() );

    fiona::timer timer( client.get_executor() );
    co_await timer.async_wait( client_sleep_dur );

    co_await client.async_send( sv );
    CHECK( mbytes_transferred.value() == sv.size() );

    ++num_runs;
    co_return;
  };

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ex, &addr );

  auto port = acceptor.port();

  ex.spawn( server( std::move( acceptor ) ) );
  ex.spawn( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "recv cancel" )
{
  num_runs = 0;

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto msession = co_await acceptor.async_accept();
    auto& session = msession.value();
    session.timeout( 5s );

    std::uint16_t buffer_group_id = 0;

    auto ex = session.get_executor();
    fiona::spawn( ex, FIONA_TASK( fiona::tcp::stream session ) {
      auto ex = session.get_executor();
      fiona::timer timer( ex );
      co_await timer.async_wait( 1s );
      co_await session.async_cancel();

      ++num_runs;
      co_return;
    }( session ) );

    session.set_buffer_group( buffer_group_id );
    {
      auto mbuf = co_await session.async_recv();
      CHECK( mbuf.has_error() );
      CHECK( mbuf.error() == std::errc::operation_canceled );
    }

    ++num_runs;

    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );

    auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
    auto mok = co_await client.async_connect( &addr );
    CHECK( mok.has_value() );

    fiona::timer timer( ex );
    co_await timer.async_wait( 1500ms );

    co_await client.async_close();

    ++num_runs;
    co_return;
  };

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ex, &addr );

  auto port = acceptor.port();

  ex.spawn( server( std::move( acceptor ) ) );
  ex.spawn( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 3 );
}

TEST_CASE( "recv high traffic" )
{
  num_runs = 0;

  auto server = []( fiona::tcp::acceptor acceptor,
                    fiona::executor /* ex */ ) -> fiona::task<void> {
    auto msession = co_await acceptor.async_accept();
    auto& session = msession.value();
    session.timeout( std::chrono::seconds( 1 ) );

    session.set_buffer_group( 0 );

    for ( int i = 0; i < 4; ++i ) {
      auto mbuf = co_await session.async_recv();
      CHECK( mbuf.has_value() );

      fiona::timer timer( session.get_executor() );
      co_await timer.async_wait( 500ms );
    }

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );

    auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
    auto mok = co_await client.async_connect( &addr );
    CHECK( mok.has_value() );

    auto const sv = std::string_view( "rawr" );

    fiona::timer timer( ex );
    for ( int i = 0; i < 4; ++i ) {
      co_await client.async_send( sv );
      co_await timer.async_wait( 500ms );
    }

    ++num_runs;
    co_return;
  };

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ex, &addr );

  auto port = acceptor.port();

  ex.spawn( server( std::move( acceptor ), ex ) );
  ex.spawn( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "double registering same buffer group" )
{
  fiona::io_context ioc;

  std::uint16_t const buffer_group_id = 0;
  ioc.register_buffer_sequence( 16, 64, buffer_group_id );
  try {
    ioc.register_buffer_sequence( 32, 128, buffer_group_id );
    CHECK( false );
  } catch ( std::system_error const& ec ) {
    CHECK( ec.code() == std::errc::file_exists );
  }
}

TEST_CASE( "buffer exhaustion" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  constexpr static int const num_bufs = 8;

  ioc.register_buffer_sequence( num_bufs, 64, 0 );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ex, &addr );
  auto const port = acceptor.port();

  static std::array<unsigned char, 64> const msg = { '\x12' };

  ioc.spawn( FIONA_TASK( fiona::tcp::acceptor acceptor ) {
    auto mstream = co_await acceptor.async_accept();
    auto& stream = mstream.value();
    stream.timeout( 500ms );

    auto mstream2 = co_await acceptor.async_accept();
    auto& stream2 = mstream2.value();
    stream2.timeout( 500ms );

    stream.set_buffer_group( 0 );
    stream2.set_buffer_group( 0 );

    std::vector<fiona::recv_buffer_sequence> buffer_sequences;

    {
      // resumption should replenish the buf ring
      auto mbufs = co_await stream.async_recv();
      auto bufs = std::move( mbufs ).value();
      CHECK( std::distance( bufs.begin(), bufs.end() ) == num_bufs );
      for ( auto buf_view : bufs ) {
        CHECK( buf_view.size() == 64 );
      }

      buffer_sequences.push_back( std::move( bufs ) );
    }

    {
      // attempting to co_await again should at least propagate the error that
      // we, at one point in time, ran out of buffer space
      auto mbufs = co_await stream.async_recv();
      CHECK( mbufs.error() == std::errc::no_buffer_space );
    }

    auto ex = stream.get_executor();
    ex.register_buffer_sequence( num_bufs, 64, 1 );

    {
      // a recv here should succeed with the replenished buffers
      auto mbufs = co_await stream2.async_recv();
      auto bufs = std::move( mbufs ).value();
      CHECK( std::distance( bufs.begin(), bufs.end() ) == num_bufs );
      for ( auto buf_view : bufs ) {
        CHECK( buf_view.size() == 64 );
      }

      buffer_sequences.push_back( std::move( bufs ) );
    }

    stream.set_buffer_group( 1 );
    {
      auto mbufs = co_await stream.async_recv();
      auto bufs = std::move( mbufs ).value();
      CHECK( std::distance( bufs.begin(), bufs.end() ) == num_bufs );
      for ( auto buf_view : bufs ) {
        CHECK( buf_view.size() == 64 );
      }
      buffer_sequences.push_back( std::move( bufs ) );
    }

    {
      // attempting to co_await again should at least propagate the error that
      // we, at one point in time, ran out of buffer space
      auto mbufs = co_await stream2.async_recv();
      CHECK( mbufs.error() == std::errc::no_buffer_space );
    }

    {
      // a recv here should succeed with the replenished buffers
      auto mbufs = co_await stream2.async_recv();
      auto bufs = std::move( mbufs ).value();
      CHECK( std::distance( bufs.begin(), bufs.end() ) == num_bufs );
      for ( auto buf_view : bufs ) {
        CHECK( buf_view.size() == 64 );
      }
      buffer_sequences.push_back( std::move( bufs ) );
    }

    ++num_runs;
  }( std::move( acceptor ) ) );

  auto client = FIONA_TASK( fiona::executor ex, std::uint16_t const port )
  {
    fiona::tcp::client client( ex );

    auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
    co_await client.async_connect( &addr );

    std::vector<unsigned char> send_buf;

    for ( int i = 0; i < 2 * num_bufs; ++i ) {
      send_buf.insert( send_buf.end(), msg.begin(), msg.end() );
    }
    CHECK( send_buf.size() == 2 * num_bufs * msg.size() );

    auto mbytes_transferred = co_await client.async_send( send_buf );
    CHECK( mbytes_transferred.value() == send_buf.size() );

    fiona::timer timer( ex );
    co_await timer.async_wait( 250ms );

    ++num_runs;
  };

  ioc.spawn( client( ex, port ) );
  ioc.spawn( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 3 );
}

TEST_CASE( "concurrent send and recv" )
{
  constexpr static std::string_view const server_msg =
      "hello world from the server!";
  constexpr static std::string_view const client_msg =
      "the client says rawr!!!!";
  // try to keep the number of clients relatively contained here
  // we're only using a single thread and we're launching all the clients in a
  // single pass. this can overwhelm the listening socket's accept backlog and
  // connections will start getting closed in debug builds w/ sanitizers
  // maybe pull this into two threads someday if 500 clients isn't enough
  constexpr static int const num_clients = 500;
  constexpr static int const num_msgs = 100;

  struct server_op
  {
    static fiona::task<void> start( fiona::tcp::acceptor acceptor,
                                    int const num_clients )
    {
      for ( int i = 0; i < num_clients; ++i ) {
        auto m_stream = co_await acceptor.async_accept();
        CHECK( m_stream.has_value() );

        auto stream = m_stream.value();
        auto ex = stream.get_executor();
        ex.spawn( session( stream ) );
      }
      co_return;
    }

    static fiona::task<void> send_server_msg( fiona::tcp::stream stream,
                                              int idx )
    {
      if ( idx == 0 ) {
        co_return;
      }

      auto ex = stream.get_executor();
      auto m_sent = co_await stream.async_send( server_msg );
      CHECK( m_sent.has_value() );
      CHECK( m_sent.value() == server_msg.size() );
      ex.spawn( send_server_msg( stream, idx - 1 ) );
      co_return;
    }

    static fiona::task<void> session( fiona::tcp::stream stream )
    {
      auto ex = stream.get_executor();
      stream.set_buffer_group( 0 );

      ex.spawn( send_server_msg( stream, num_msgs ) );

      fiona::recv_buffer_sequence bufs;
      std::size_t num_read = 0;

      while ( num_read < ( num_msgs * client_msg.size() ) ) {
        auto m_bufs = co_await stream.async_recv();
        CHECK( m_bufs.has_value() );

        for ( auto buf : m_bufs.value() ) {
          num_read += buf.size();
        }
        bufs.concat( std::move( m_bufs ).value() );
      }

      fiona::timer timer( ex );
      co_await timer.async_wait( 500ms );

      co_await stream.async_cancel();
      co_await stream.async_close();

      ++num_runs;
      co_return;
    }
  };

  struct client_op
  {
    static fiona::task<void> start( fiona::executor ex,
                                    std::uint16_t const port )
    {
      fiona::tcp::client client( ex );

      auto ipv4_addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
      auto m_ok = co_await client.async_connect( &ipv4_addr );
      CHECK( m_ok.has_value() );

      fiona::tcp::stream stream = client;
      stream.set_buffer_group( 1 );

      ex.spawn( send_client_msg( stream, num_msgs ) );

      fiona::recv_buffer_sequence bufs;
      std::size_t num_read = 0;
      while ( num_read < ( num_msgs * server_msg.size() ) ) {
        auto m_bufs = co_await stream.async_recv();
        CHECK( m_bufs.has_value() );
        if ( m_bufs.has_error() ) {
          CHECK( num_read == 0 );
        }

        for ( auto buf : m_bufs.value() ) {
          num_read += buf.size();
        }
        bufs.concat( std::move( m_bufs ).value() );
      }

      fiona::timer timer( ex );
      co_await timer.async_wait( 500ms );

      co_await stream.async_cancel();
      co_await stream.async_close();

      ++num_runs;
      co_return;
    }

    static fiona::task<void> send_client_msg( fiona::tcp::stream stream,
                                              int idx )
    {
      if ( idx == 0 ) {
        co_return;
      }

      auto ex = stream.get_executor();
      auto m_sent = co_await stream.async_send( client_msg );
      CHECK( m_sent.has_value() );
      CHECK( m_sent.value() == client_msg.size() );
      ex.spawn( client_op::send_client_msg( stream, idx - 1 ) );
      co_return;
    }
  };

  num_runs = 0;

  fiona::io_context_params params;
  params.num_files = 4 * 1024;
  fiona::io_context ioc( params );
  auto ex = ioc.get_executor();

  auto ipv4_addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ex, &ipv4_addr );

  auto port = acceptor.port();

  ex.register_buffer_sequence( 1024, 128, 0 );
  ex.register_buffer_sequence( 1024, 128, 1 );

  ioc.spawn( server_op::start( acceptor, num_clients ) );
  for ( int i = 0; i < num_clients; ++i ) {
    ioc.spawn( client_op::start( ioc.get_executor(), port ) );
  }
  ioc.run();

  CHECK( num_runs == 2 * num_clients );
}
