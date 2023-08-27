#include <fiona/error.hpp>
#include <fiona/io_context.hpp>
#include <fiona/sleep.hpp>
#include <fiona/tcp.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <cstdint>
#include <string_view>

CATCH_TRANSLATE_EXCEPTION( fiona::error_code const& ex ) {
  return ex.message();
}

constexpr std::uint32_t localhost = 0x7f000001;

static int num_runs = 0;

TEST_CASE( "accept sanity test" ) {
  num_runs = 0;

  fiona::io_context ioc;
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );
  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto a = acceptor.async_accept();

    auto stream = co_await a;

    auto r = stream.value().async_recv( 0 );
    auto buf = co_await r;

    auto octets = buf.value().readable_bytes();
    auto str = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() - 1 );
    CHECK( octets.size() > 0 );
    CHECK( str == "hello, world!" );

    auto n_result =
        co_await stream.value().async_write( octets.data(), octets.size() );

    CHECK( n_result.value() == octets.size() );

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    auto client_result = co_await fiona::tcp::client::make( ex );
    auto& client = client_result.value();

    auto ec = co_await client.async_connect( localhost, port );
    CHECK( !ec );

    co_await fiona::sleep_for( ex, std::chrono::seconds( 1 ) );

    char const msg[] = "hello, world!";
    auto result = co_await client.async_write( msg, std::size( msg ) );
    CHECK( result.value() == std::size( msg ) );

    ++num_runs;
  };

  ioc.post( server( std::move( acceptor ) ) );
  ioc.post( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "accept back-pressure test" ) {
  // this test purposefully doesn't exceed the size of the completion queue so
  // that our multishot accept() doesn't need to be rescheduled

  constexpr std::size_t num_clients = 100;

  num_runs = 0;

  fiona::io_context ioc;
  REQUIRE( num_clients < ioc.params().cq_entries );
  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );
  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor,
                    fiona::executor ex ) -> fiona::task<void> {
    auto a = acceptor.async_accept();

    {
      // actually start the multishot accept sqe
      auto stream = co_await a;
      (void)stream;
    }

    co_await fiona::sleep_for( ex, std::chrono::milliseconds( 200 ) );

    for ( unsigned i = 1; i < num_clients; ++i ) {
      // ideally, this doesn't suspend at all and instead we just start pulling
      // from the queue of waiting connections
      auto stream = co_await a;

      CHECK( stream.has_value() );
    }

    ++num_runs;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    auto client_result = co_await fiona::tcp::client::make( ex );
    auto& client = client_result.value();

    auto ec = co_await client.async_connect( localhost, port );
    CHECK( !ec );

    ++num_runs;
  };

  ioc.post( server( std::move( acceptor ), ex ) );
  for ( unsigned i = 0; i < num_clients; ++i ) {
    ex.post( client( ex, port ) );
  }

  ioc.run();

  CHECK( num_runs == num_clients + 1 );
}

TEST_CASE( "accept CQ overflow" ) {
  // this test purposefully exceeds the size of the completion queue so
  // that our multishot accept() needs to be rescheduled

  // this number will roughly double because of the one-to-one correspondence
  // between a connect() CQE and the accept() CQE
  constexpr std::size_t num_clients = 600;

  num_runs = 0;

  fiona::io_context_params params{
      .sq_entries = 512, .cq_entries = 1024, .num_files = 2000 };

  fiona::io_context ioc( params );
  REQUIRE( 2 * num_clients >= ioc.params().cq_entries );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );

  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor,
                    fiona::executor ex ) -> fiona::task<void> {
    auto a = acceptor.async_accept();

    {
      auto stream = co_await a;
      CHECK( stream.has_value() );
    }

    co_await fiona::sleep_for( ex, std::chrono::milliseconds( 200 ) );
    for ( unsigned i = 1; i < num_clients; ++i ) {
      auto stream = co_await a;
      CHECK( stream.has_value() );
    }

    ++num_runs;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    auto client_result = co_await fiona::tcp::client::make( ex );
    auto& client = client_result.value();
    client.timeout( std::chrono::seconds( 3 ) );
    auto ec = co_await client.async_connect( localhost, port );
    (void)ec;
    // CHECK( !ec );
    // if ( ec ) {
    //   throw ec;
    // }
    ++num_runs;
  };

  ex.post( server( std::move( acceptor ), ex ) );
  for ( unsigned i = 0; i < num_clients; ++i ) {
    ex.post( client( ex, port ) );
  }

  ioc.run();

  CHECK( num_runs == num_clients + 1 );
}

TEST_CASE( "client connection refused" ) {
  num_runs = 0;

  auto client = []( fiona::executor ex ) -> fiona::task<void> {
    auto client_result = co_await fiona::tcp::client::make( ex );
    auto& client = client_result.value();
    client.timeout( std::chrono::seconds( 2 ) );

    auto ec = co_await client.async_connect( localhost, 3301 );

    CHECK( ec );
    CHECK( ec == fiona::error_code::from_errno( ECONNREFUSED ) );

    ++num_runs;
    co_return;
  };

  constexpr int num_clients = 100;

  fiona::io_context ioc;
  for ( int i = 0; i < num_clients; ++i ) {
    ioc.post( client( ioc.get_executor() ) );
  }
  ioc.run();
  CHECK( num_runs == num_clients );
}

TEST_CASE( "client connect timeout" ) {
  num_runs = 0;

  auto client = []( fiona::executor ex ) -> fiona::task<void> {
    auto client_result = co_await fiona::tcp::client::make( ex );
    auto& client = client_result.value();
    client.timeout( std::chrono::seconds( 2 ) );

    // use one of the IP addresses from the test networks:
    // 192.0.2.0/24
    // https://en.wikipedia.org/wiki/Internet_Protocol_version_4#Special-use_addresses
    auto ec = co_await client.async_connect( 0xc0'00'02'00, 3301 );

    CHECK( ec );
    CHECK( ec == fiona::error_code::from_errno( ECANCELED ) );

    ++num_runs;
    co_return;
  };

  constexpr int num_clients = 100;

  fiona::io_context ioc;
  for ( int i = 0; i < num_clients; ++i ) {
    ioc.post( client( ioc.get_executor() ) );
  }
  ioc.run();
  CHECK( num_runs == num_clients );
}

TEST_CASE( "tcp echo" ) {
  num_runs = 0;
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

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );
  auto const port = acceptor.port();

  auto handle_request = []( fiona::executor, fiona::tcp::stream stream,
                            std::string_view msg ) -> fiona::task<void> {
    std::size_t num_bytes = 0;

    auto rx = stream.async_recv( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto borrowed_buf = co_await rx;

      auto octets = borrowed_buf.value().readable_bytes();
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      CHECK( m == msg );

      auto num_written =
          co_await stream.async_write( octets.data(), octets.size() );

      CHECK( !num_written.has_error() );
      CHECK( num_written.value() == octets.size() );
      num_bytes += octets.size();
    }

    ++num_runs;
  };

  auto server = [handle_request]( fiona::executor ex,
                                  fiona::tcp::acceptor acceptor,
                                  std::string_view msg ) -> fiona::task<void> {
    auto a = acceptor.async_accept();

    for ( int i = 0; i < num_clients; ++i ) {
      auto stream = co_await a;
      ex.post( handle_request( ex, std::move( stream.value() ), msg ) );
    }

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex, std::uint16_t port,
                    std::string_view msg ) -> fiona::task<void> {
    auto client_result = co_await fiona::tcp::client::make( ex );
    auto& client = client_result.value();

    client.timeout( std::chrono::seconds( 3 ) );

    auto ec = co_await client.async_connect( localhost, port );
    CHECK( !ec );

    std::size_t num_bytes = 0;

    auto rx = client.async_recv( bgid );

    while ( num_bytes < num_msgs * msg.size() ) {
      auto result = co_await client.async_write( msg.data(), msg.size() );
      CHECK( result.value() == std::size( msg ) );

      auto borrowed_buf = co_await rx;

      auto octets = borrowed_buf.value().readable_bytes();
      auto m = std::string_view( reinterpret_cast<char const*>( octets.data() ),
                                 octets.size() );
      CHECK( m == msg );

      num_bytes += octets.size();
    }

    ++num_runs;
  };

  ioc.post( server( ex, std::move( acceptor ), msg ) );

  for ( int i = 0; i < num_clients; ++i ) {
    ioc.post( client( ex, port, msg ) );
  }

  ioc.run();

  CHECK( num_runs == 1 + ( 2 * num_clients ) );
}

TEST_CASE( "fd reuse" ) {
  num_runs = 0;
  // want to prove that the runtime correctly manages its set of file
  // descriptors by proving they can be reused over and over again

  constexpr std::uint32_t num_files = 20;
  constexpr std::uint32_t total_connections = 4 * num_files;

  fiona::io_context_params params;
  params.num_files = num_files;

  fiona::io_context ioc( params );
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );
  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto a = acceptor.async_accept();

    std::uint32_t num_accepted = 0;

    while ( num_accepted < total_connections ) {
      auto stream = co_await a;
      ++num_accepted;

      auto r = stream.value().async_recv( 0 );
      auto buf = co_await r;

      auto octets = buf.value().readable_bytes();
      auto str = std::string_view(
          reinterpret_cast<char const*>( octets.data() ), octets.size() - 1 );
      CHECK( octets.size() > 0 );
      CHECK( str == "hello, world!" );

      auto n_result =
          co_await stream.value().async_write( octets.data(), octets.size() );

      CHECK( n_result.value() == octets.size() );
    }

    ++num_runs;
    co_return;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    for ( std::uint32_t i = 0; i < total_connections; ++i ) {
      auto client_result = co_await fiona::tcp::client::make( ex );
      auto& client = client_result.value();

      auto ec = co_await client.async_connect( localhost, port );
      CHECK( !ec );

      char const msg[] = "hello, world!";
      auto result = co_await client.async_write( msg, std::size( msg ) );
      CHECK( result.value() == std::size( msg ) );
    }
    ++num_runs;
  };

  ioc.post( server( std::move( acceptor ) ) );
  ioc.post( client( ex, port ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

// things to test:
// * removing buffer groups during async ops
// * buffer exhausting
// * recv timeouts
// * exceptions being thrown at random times
// * update accept() to be more result-oriented instead of just `i32`
