#include "fiona/detail/common.hpp"
#include "fiona/error.hpp"
#include "helpers.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

static int num_runs = 0;

TEST_CASE( "recv_test - recv timeout" ) {
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
      if ( mbuf.has_error() ) {
        CHECK( mbuf.error() == fiona::error_code::from_errno( EBADF ) );
      }
    }

    ++num_runs;

    co_return;
  };

  auto client = []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
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

TEST_CASE( "recv_test - recv cancel" ) {
  num_runs = 0;

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto msession = co_await acceptor.async_accept();
    auto& session = msession.value();
    session.timeout( 5s );

    std::uint16_t buffer_group_id = 0;

    auto ex = session.get_executor();
    fiona::spawn(
        ex, FIONA_TASK( fiona::tcp::stream session ) {
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

  auto client = []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
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

TEST_CASE( "recv_test - recv high traffic" ) {
  num_runs = 0;

  auto server = []( fiona::tcp::acceptor acceptor, fiona::executor /* ex */ ) -> fiona::task<void> {
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

  auto client = []( fiona::executor ex, std::uint16_t port ) -> fiona::task<void> {
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

TEST_CASE( "recv_test - double registering same buffer group" ) {
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

// TEST_CASE( "recv_test - buffer exhaustion" ) {
//   num_runs = 0;

//   fiona::io_context ioc;
//   auto ex = ioc.get_executor();

//   constexpr static int const num_bufs = 8;

//   ioc.register_buffer_sequence( num_bufs, 64, 0 );

//   auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
//   fiona::tcp::acceptor acceptor( ex, &addr );
//   auto const port = acceptor.port();

//   static std::array<unsigned char, 64> const msg = { '\x12' };

//   ioc.spawn( FIONA_TASK( fiona::tcp::acceptor acceptor ) {
//     auto mstream = co_await acceptor.async_accept();
//     auto& stream = mstream.value();
//     stream.timeout( 500ms );

//     auto mstream2 = co_await acceptor.async_accept();
//     auto& stream2 = mstream2.value();
//     stream2.timeout( 500ms );

//     stream.set_buffer_group( 0 );
//     stream2.set_buffer_group( 0 );

//     std::vector<fiona::borrowed_buffer> bufs;

//     {
//       for ( int i = 0; i < 2 * num_bufs; ++i ) {
//         auto mbuf = co_await stream.async_recv();
//         if ( i >= num_bufs ) {
//           CHECK( bufs.size() >= num_bufs );
//           CHECK( mbuf.error() == std::errc::no_buffer_space );
//         } else {
//           CHECK( mbuf.has_value() );
//           bufs.push_back( std::move( mbuf.value() ) );
//         }
//       }
//     }

//     auto ex = stream.get_executor();
//     ex.register_buffer_sequence( num_bufs, 64, 1 );

//     {
//       auto mbuf = co_await stream2.async_recv();
//       CHECK( mbuf.has_error() );
//       CHECK( mbuf.error() == std::errc::no_buffer_space );
//     }

//     stream.set_buffer_group( 1 );
//     {
//       std::size_t num_read = 0;
//       while ( num_read < msg.size() * num_bufs ) {
//         auto mbuf = co_await stream.async_recv();
//         CHECK( mbuf.has_value() );
//         num_read += mbuf.value().readable_bytes().size();
//       }
//     }

//     bufs.clear();
//     {
//       std::size_t num_read = 0;
//       while ( num_read < msg.size() * 2 * num_bufs ) {
//         auto mbuf = co_await stream2.async_recv();
//         if ( mbuf.has_value() ) {
//           num_read += mbuf.value().readable_bytes().size();
//         } else {
//           // interesting to note that we can't seem to avoid the interruption to the scheduled recv operation
//           // this could be a ux flaw in the library itself but maybe it's useful for the user to know if their buffer
//           // group overflowed
//           CHECK( mbuf.error() == std::errc::no_buffer_space );
//         }
//       }
//     }

//     ++num_runs;
//   }( std::move( acceptor ) ) );

//   auto client = FIONA_TASK( fiona::executor ex, std::uint16_t const port ) {
//     fiona::tcp::client client( ex );

//     auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
//     co_await client.async_connect( &addr );

//     for ( int i = 0; i < 2 * num_bufs; ++i ) {
//       auto mbytes_transferred = co_await client.async_send( msg );
//       CHECK( mbytes_transferred.value() == msg.size() );
//     }

//     fiona::timer timer( ex );
//     co_await timer.async_wait( 250ms );

//     ++num_runs;
//   };

//   ioc.spawn( client( ex, port ) );
//   ioc.spawn( client( ex, port ) );

//   ioc.run();

//   CHECK( num_runs == 3 );
// }
