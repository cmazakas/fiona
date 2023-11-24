#include "helpers.hpp"

#include <fiona/io_context.hpp>
#include <fiona/tcpv2.hpp>
#include <fiona/time.hpp>

#include <atomic>
#include <random>
#include <thread>

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

  constexpr static auto const client_msg =
      std::string_view( "hello, world! from the client" );

  constexpr static auto const server_msg =
      std::string_view( "hello, world! from the server" );

  ioc.post( FIONA_TASK( fiona::tcp::acceptor acceptor ) {
    auto mstream = co_await acceptor.async_accept();
    CHECK( mstream.has_value() );

    auto& stream = mstream.value();

    auto mbytes_transferred = co_await stream.async_send( server_msg );

    CHECK( static_cast<std::size_t>( mbytes_transferred.value() ) ==
           server_msg.size() );

    auto rx = stream.get_receiver( 0 );

    auto mbuffer = co_await rx.async_recv();
    CHECK( mbuffer.has_value() );

    auto& buffer = mbuffer.value();
    auto msg = buffer.as_str();
    CHECK( msg == client_msg );

    ++num_runs;
    co_return;
  }( std::move( acceptor ) ) );

  ioc.post( FIONA_TASK( fiona::executor ex, std::uint16_t const port ) {
    fiona::tcp::client client( ex );
    auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
    CHECK( mok.has_value() );

    auto mbytes_transferred = co_await client.async_send( client_msg );
    CHECK( static_cast<std::size_t>( mbytes_transferred.value() ) ==
           client_msg.size() );

    auto rx = client.get_receiver( 0 );
    auto mbuffer = co_await rx.async_recv();
    CHECK( mbuffer.has_value() );

    auto& buffer = mbuffer.value();
    auto msg = buffer.as_str();
    CHECK( msg == server_msg );

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

TEST_CASE( "tcp2_test - tcp echo" ) {
  static std::atomic_uint64_t anum_runs = 0;
  constexpr int num_clients = 500;
  constexpr int num_msgs = 1000;
  constexpr std::uint16_t bgid = 0;

  constexpr std::string_view msg = "hello, world!";

  fiona::io_context_params params;
  params.num_files = 1024;
  params.sq_entries = 4096;
  params.cq_entries = 4096;

  fiona::io_context ioc( params );
  ioc.register_buffer_sequence( 1024, 128, bgid );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();

  auto handle_request = []( fiona::executor, fiona::tcp::stream stream,
                            std::string_view msg ) -> fiona::task<void> {
    stream.timeout( std::chrono::seconds( 5 ) );

    std::size_t num_bytes = 0;

    auto rx = stream.get_receiver( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto borrowed_buf = co_await rx.async_recv();
      CHECK( borrowed_buf.has_value() );

      auto octets = borrowed_buf.value().readable_bytes();
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      CHECK( m == msg );

      auto num_written = co_await stream.async_send( octets );

      CHECK( !num_written.has_error() );
      CHECK( static_cast<std::size_t>( num_written.value() ) == octets.size() );
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

    CHECK( mok.has_value() );

    std::size_t num_bytes = 0;

    auto rx = client.get_receiver( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto result = co_await client.async_send( msg );
      CHECK( static_cast<std::size_t>( result.value() ) == std::size( msg ) );

      auto borrowed_buf = co_await rx.async_recv();

      auto octets = borrowed_buf.value().readable_bytes();
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      CHECK( m == msg );

      num_bytes += octets.size();
    }

    ++anum_runs;
  };

  std::thread t1( [&params, &client, port, msg] {
    try {
      fiona::io_context ioc( params );
      ioc.register_buffer_sequence( 1024, 128, bgid );

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

  CHECK( anum_runs == 1 + ( 2 * num_clients ) );
}

TEST_CASE( "tcp2_test - fd reuse" ) {
  num_runs = 0;
  // want to prove that the runtime correctly manages its set of file
  // descriptors by proving they can be reused over and over again

  constexpr std::uint32_t num_files = 10;
  constexpr std::uint32_t total_connections = 4 * num_files;

  fiona::io_context_params params;
  params.num_files = 2 * num_files; // duality because of client<->server

  fiona::io_context ioc( params );
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost_ipv4, 0 );
  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    std::random_device rd;
    std::mt19937 g( rd() );

    std::uint32_t num_accepted = 0;

    std::vector<fiona::tcp::stream> sessions;

    while ( num_accepted < total_connections ) {
      auto mstream = co_await acceptor.async_accept();
      ++num_accepted;

      REQUIRE( mstream.has_value() );
      auto& stream = mstream.value();

      stream.timeout( std::chrono::seconds( 3 ) );

      auto mbuf = co_await stream.get_receiver( 0 ).async_recv();

      CHECK( mbuf.has_value() );
      if ( mbuf.has_error() ) {
        co_return;
      }

      auto& buf = mbuf.value();
      auto octets = buf.readable_bytes();
      CHECK( !octets.empty() );
      if ( octets.empty() ) {
        co_return;
      }

      auto str = std::string_view(
          reinterpret_cast<char const*>( octets.data() ), octets.size() );
      CHECK( octets.size() > 0 );
      CHECK( str == "hello, world!" );

      auto n_result = co_await stream.async_send( octets );

      CHECK( n_result.value() == octets.size() );

      sessions.push_back( std::move( stream ) );
      if ( sessions.size() == num_files ) {
        std::shuffle( sessions.begin(), sessions.end(), g );
        sessions.clear();
      }
    }

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    std::random_device rd;
    std::mt19937 g( rd() );
    std::vector<fiona::tcp::client> sessions;

    for ( std::uint32_t i = 0; i < total_connections; ++i ) {
      fiona::tcp::client client( ex );
      auto mok = co_await client.async_connect( localhost_ipv4, htons( port ) );
      mok.value();

      std::string_view msg = "hello, world!";
      auto result = co_await client.async_send( msg );
      CHECK( result.value() == std::size( msg ) );

      sessions.push_back( std::move( client ) );
      if ( sessions.size() == num_files ) {
        std::shuffle( sessions.begin(), sessions.end(), g );
        sessions.clear();
      }
    }

    ++num_runs;
  };

  ioc.post( server( std::move( acceptor ) ) );
  ioc.post( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}
