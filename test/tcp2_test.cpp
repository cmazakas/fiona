#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/tcpv2.hpp>
#include <fiona/time.hpp>

static int num_runs = 0;

TEST_CASE( "tcp2_test - acceptor" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );
    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );
    ++num_runs;
    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "tcp2_test - client already connected" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );

    fiona::timer timer( acceptor.get_executor() );
    co_await timer.async_wait( std::chrono::milliseconds( 500 ) );

    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    {
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      CHECK( mok.has_value() );
    }

    auto timer = fiona::timer( ex );
    co_await timer.async_wait( std::chrono::milliseconds( 100 ) );

    {
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      CHECK( mok.has_error() );
      CHECK( mok.error() == fiona::error_code::from_errno( EISCONN ) );
    }

    {
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      CHECK( mok.has_error() );
      CHECK( mok.error() == fiona::error_code::from_errno( EISCONN ) );
    }

    ++num_runs;

    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "tcp2_test - server not listening" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_error() );
    CHECK( mok.error() == fiona::error_code::from_errno( ECONNREFUSED ) );

    ++num_runs;
    co_return;
  }( ex, 3333 ) );

  ioc.run();

  CHECK( num_runs == 1 );
}

TEST_CASE( "tcp2_test - client connect timeout" ) {
  num_runs = 0;

  // use one of the IP addresses from the test networks:
  // 192.0.2.0/24
  // https://en.wikipedia.org/wiki/Internet_Protocol_version_4#Special-use_addresses
  static auto const ipv4_addr = bytes_to_ipv4( { 192, 0, 2, 0 } );

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
    auto const timeout = std::chrono::milliseconds( 1000 );

    fiona::tcp::client client( ex );
    client.timeout( timeout );

    duration_guard dg( timeout );
    auto mok = co_await client.async_connect( ipv4_addr, 3301 );
    CHECK( mok.error() == fiona::error_code::from_errno( ECANCELED ) );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();

  CHECK( num_runs == 1 );
}

TEST_CASE( "tcp2_test - client connect interruption" ) {
  num_runs = 0;

  // use one of the IP addresses from the test networks:
  // 192.0.2.0/24
  // https://en.wikipedia.org/wiki/Internet_Protocol_version_4#Special-use_addresses
  static auto const ipv4_addr = bytes_to_ipv4( { 192, 0, 2, 0 } );

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
    auto const timeout = std::chrono::milliseconds( 10'000 );

    fiona::tcp::client client( ex );
    client.timeout( timeout );

    ++num_runs;
    auto mok = co_await client.async_connect( ipv4_addr, 3301 );
    (void)mok;
    CHECK( false );

    co_return;
  }( ex ) );

  ioc.post( []() -> fiona::task<void> {
    ++num_runs;
    throw 1234;
    co_return;
  }() );

  CHECK_THROWS( ioc.run() );

  CHECK( num_runs == 2 );
}

TEST_CASE( "tcp2_test - double connect" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );
    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );

    auto h = fiona::post( ex,
                          []( fiona::tcp::client client,
                              std::uint16_t port ) -> fiona::task<void> {
                            CHECK_THROWS( co_await client.async_connect(
                                localhost_ipv4, htons( port ) ) );

                            ++num_runs;
                          }( client, port ) );

    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );

    co_await h;

    ++num_runs;
    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 3 );
}

TEST_CASE( "tcp2_test - socket creation failed" ) {
  num_runs = 0;

  fiona::io_context_params params;
  params.num_files = 16;

  fiona::io_context ioc( params );
  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    std::vector<fiona::tcp::stream> streams;
    for ( int i = 0; i < 8; ++i ) {
      auto mstream = co_await acceptor.async_accept();
      streams.push_back( std::move( mstream.value() ) );
    }

    fiona::timer timer( acceptor.get_executor() );
    co_await timer.async_wait( 500ms );

    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    std::vector<fiona::tcp::client> clients;
    for ( int i = 0; i < 10; ++i ) {
      fiona::tcp::client client( ex );
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      if ( i < 8 ) {
        CHECK( mok.has_value() );
        clients.push_back( std::move( client ) );
      } else {
        CHECK( mok.has_error() );
        CHECK( mok.error() == fiona::error_code::from_errno( ENFILE ) );
      }
    }

    ++num_runs;
    co_return;
  }( ex, port ) );

  ioc.run();
  CHECK( num_runs == 2 );
}

TEST_CASE( "tcp2_test - connect cancellation" ) {
  num_runs = 0;

  // use one of the IP addresses from the test networks:
  // 192.0.2.0/24
  // https://en.wikipedia.org/wiki/Internet_Protocol_version_4#Special-use_addresses
  static auto const ipv4_addr = bytes_to_ipv4( { 192, 0, 2, 0 } );

  fiona::io_context ioc;

  fiona::tcp::acceptor acceptor( ioc.get_executor(), localhost_ipv4, 0 );
  auto const port = acceptor.port();

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );
    ++num_runs;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    client.timeout( 10s );

    auto h =
        fiona::post( ex, []( fiona::tcp::client client ) -> fiona::task<void> {
          fiona::timer timer( client.get_executor() );
          co_await timer.async_wait( 2s );

          auto mcancelled = co_await client.async_cancel();
          CHECK( mcancelled.value() == 1 );
          ++num_runs;
          co_return;
        }( client ) );

    {
      duration_guard dg( 2s );
      auto mok = co_await client.async_connect( ipv4_addr, htons( 3300 ) );
      CHECK( mok.error() == fiona::error_code::from_errno( ECANCELED ) );
    }

    co_await h;

    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );

    ++num_runs;
    co_return;
  }( ioc.get_executor(), port ) );
  ioc.run();

  CHECK( num_runs == 3 );
}

TEST_CASE( "tcp2_test - client reconnection" ) {
  num_runs = 0;

  fiona::io_context ioc;

  fiona::tcp::acceptor acceptor( ioc.get_executor(), localhost_ipv4, 0 );
  auto const port = acceptor.port();

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    std::vector<fiona::tcp::stream> connected;

    for ( int i = 0; i < 3; ++i ) {
      auto mstream = co_await acceptor.async_accept();
      connected.push_back( std::move( mstream.value() ) );
    }

    fiona::timer timer( acceptor.get_executor() );
    co_await timer.async_wait( 500ms );

    for ( auto& stream : connected ) {
      auto m_ok = co_await stream.async_close();
      CHECK( m_ok.has_value() );
    }

    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    for ( int i = 0; i < 3; ++i ) {
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      CHECK( mok.has_value() );

      mok = co_await client.async_close();
      CHECK( mok.has_value() );
    }

    ++num_runs;
    co_return;
  }( ioc.get_executor(), port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "tcp2_test - send recv hello world" ) {

  num_runs = 0;

  fiona::io_context ioc;
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );

    auto& stream = mstream.value();
    auto sv = std::string_view( "hello, world!" );
    auto mbytes_transferred = co_await stream.async_send( sv );

    CHECK( static_cast<std::size_t>( mbytes_transferred.value() ) ==
           sv.size() );

    fiona::timer timer( stream.get_executor() );
    co_await timer.async_wait( 250ms );

    auto mbuffer = co_await stream.async_recv( 0 );
    CHECK( mbuffer.has_value() );

    auto& buffer = mbuffer.value();
    auto msg = buffer.as_str();
    CHECK( msg == sv );

    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );

    auto sv = std::string_view( "hello, world!" );
    auto mbytes_transferred = co_await client.async_send( sv );
    CHECK( static_cast<std::size_t>( mbytes_transferred.value() ) ==
           sv.size() );

    fiona::timer timer( client.get_executor() );
    co_await timer.async_wait( 250ms );

    ++num_runs;
    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "tcp2_test - send not connected" ) {

  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();
  CHECK( port > 0 );

  // test a send with a socket that isn't connected at all
  //

  ioc.post( FIONA_TASK( fiona::executor ex, std::uint16_t const /* port  */ ) {
    fiona::tcp::client client( ex );

    auto sv = std::string_view( "hello, world!" );
    auto mbytes_transferred = co_await client.async_send( sv );
    CHECK( mbytes_transferred.has_error() );
    CHECK( mbytes_transferred.error() ==
           fiona::error_code::from_errno( EBADF ) );

    ++num_runs;
    co_return;
  }( ex, port ) );

  // now we wanna test a send when the remote has closed on us
  //

  ioc.post( []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );

    auto& stream = mstream.value();
    co_await stream.async_close();

    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( []( fiona::executor ex,
                std::uint16_t const port ) -> fiona::task<void> {
    fiona::tcp::client client( ex );
    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );

    fiona::timer timer( client.get_executor() );
    co_await timer.async_wait( 250ms );

    // for more info on why the first send() succeeds, see this answer:
    // https://stackoverflow.com/questions/11436013/writing-to-a-closed-local-tcp-socket-not-failing
    //

    auto sv = std::string_view( "hello, world!" );

    {
      auto mbytes_transferred = co_await client.async_send( sv );
      CHECK( static_cast<std::size_t>( mbytes_transferred.value() ) ==
             sv.size() );
    }

    {
      auto mbytes_transferred = co_await client.async_send( sv );
      CHECK( mbytes_transferred.has_error() );
      CHECK( mbytes_transferred.error() ==
             fiona::error_code::from_errno( EPIPE ) );
    }

    ++num_runs;
    co_return;
  }( ex, port ) );

  ioc.run();

  CHECK( num_runs == 3 );
}
