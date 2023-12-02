#include "helpers.hpp"

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
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

    std::uint16_t buffer_group_id = 0;
    auto rx = session.get_receiver( buffer_group_id );

    {
      auto mbuf = co_await rx.async_recv();
      CHECK( mbuf.has_value() );
    }

    {
      auto mbuf = co_await rx.async_recv();
      CHECK( mbuf.has_error() );
      CHECK( mbuf.error() == fiona::error_code::from_errno( ETIMEDOUT ) );
    }

    fiona::timer timer( session.get_executor() );
    co_await timer.async_wait( server_timeout );

    {
      auto mbuf = co_await rx.async_recv();
      CHECK( mbuf.has_value() );
      if ( mbuf.has_error() ) {
        CHECK( mbuf.error() == fiona::error_code::from_errno( EBADF ) );
      }
    }

    ++num_runs;

    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );

    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
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

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );

  auto port = acceptor.port();

  ex.post( server( std::move( acceptor ) ) );
  ex.post( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "recv_test - recv high traffic" ) {
  num_runs = 0;

  auto server = []( fiona::tcp::acceptor acceptor,
                    fiona::executor /* ex */ ) -> fiona::task<void> {
    auto msession = co_await acceptor.async_accept();
    auto& session = msession.value();
    session.timeout( std::chrono::seconds( 1 ) );

    auto rx = session.get_receiver( 0 );

    for ( int i = 0; i < 4; ++i ) {
      auto mbuf = co_await rx.async_recv();
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

    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
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

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );

  auto port = acceptor.port();

  ex.post( server( std::move( acceptor ), ex ) );
  ex.post( client( ex, port ) );

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

//   fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
//   auto const port = acceptor.port();

//   ioc.post( FIONA_TASK( fiona::tcp::acceptor acceptor ) {
//     auto mstream = co_await acceptor.async_accept();
//     auto& stream = mstream.value();

//     auto receiver = stream.get_receiver( 0 );

//     std::vector<fiona::borrowed_buffer> bufs;

//     for ( int i = 0; i < 2 * num_bufs; ++i ) {
//       auto mbuf = co_await receiver.async_recv();
//       if ( i >= ( num_bufs - 1 ) ) {
//         CHECK( mbuf.error() == std::errc::no_buffer_space );
//       } else {
//         CHECK( mbuf.has_value() );
//         bufs.push_back( std::move( mbuf.value() ) );
//       }
//     }

//     ++num_runs;
//   }( std::move( acceptor ) ) );

//   ioc.post( FIONA_TASK( fiona::executor ex, std::uint16_t const port ) {
//     fiona::tcp::client client( ex );
//     co_await client.async_connect( localhost_ipv4, htons( port ) );

//     for ( int i = 0; i < 2 * num_bufs; ++i ) {
//       co_await client.async_send( { "rawr" } );
//     }

//     ++num_runs;
//   }( ex, port ) );

//   ioc.run();

//   CHECK( num_runs == 2 );
// }
