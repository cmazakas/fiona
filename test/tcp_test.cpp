#include <fiona/error.hpp>
#include <fiona/io_context.hpp>
#include <fiona/sleep.hpp>
#include <fiona/tcp.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>
#include <string_view>
#include <thread>

CATCH_TRANSLATE_EXCEPTION( fiona::error_code const& ex ) {
  return ex.message();
}

constexpr in_addr localhost = in_addr{ .s_addr = 0x7f000001 };

static int num_runs = 0;

TEST_CASE( "accept sanity test" ) {
  num_runs = 0;

  fiona::io_context ioc;
  ioc.register_buffer_sequence( 1024, 128, 0 );

  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );
  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    auto stream = co_await acceptor.async_accept();

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
    auto maybe_client =
        co_await fiona::tcp::client::async_connect( ex, localhost, port );

    CHECK( maybe_client.has_value() );

    auto& client = maybe_client.value();

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
  constexpr std::size_t num_clients = 100;

  num_runs = 0;

  fiona::io_context ioc;
  REQUIRE( num_clients < ioc.params().cq_entries );
  auto ex = ioc.get_executor();

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );
  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor,
                    fiona::executor ex ) -> fiona::task<void> {
    co_await fiona::sleep_for( ex, std::chrono::milliseconds( 200 ) );

    for ( unsigned i = 1; i < num_clients; ++i ) {
      auto stream = co_await acceptor.async_accept();
      CHECK( stream.has_value() );
    }

    ++num_runs;
  };

  auto client = []( fiona::executor ex,
                    std::uint16_t port ) -> fiona::task<void> {
    auto maybe_client =
        co_await fiona::tcp::client::async_connect( ex, localhost, port );
    CHECK( maybe_client.has_value() );

    ++num_runs;
  };

  ioc.post( server( std::move( acceptor ), ex ) );
  for ( unsigned i = 0; i < num_clients; ++i ) {
    ex.post( client( ex, port ) );
  }

  ioc.run();

  CHECK( num_runs == num_clients + 1 );
}

// TEST_CASE( "accept CQ overflow" ) {
//   // this test purposefully exceeds the size of the completion queue so
//   // that our multishot accept() needs to be rescheduled

//   // this number will roughly double because of the one-to-one correspondence
//   // between a connect() CQE and the accept() CQE
//   constexpr std::size_t num_clients = 600;

//   num_runs = 0;

//   fiona::io_context_params params{
//       .sq_entries = 512, .cq_entries = 1024, .num_files = 2000 };

//   fiona::io_context ioc( params );
//   REQUIRE( 2 * num_clients >= ioc.params().cq_entries );

//   auto ex = ioc.get_executor();

//   fiona::tcp::acceptor acceptor( ex, localhost, 0 );

//   auto const port = acceptor.port();

//   auto server = []( fiona::tcp::acceptor acceptor,
//                     fiona::executor ex ) -> fiona::task<void> {
//     auto a = acceptor.async_accept();

//     {
//       auto stream = co_await a;
//       CHECK( stream.has_value() );
//     }

//     co_await fiona::sleep_for( ex, std::chrono::milliseconds( 200 ) );
//     for ( unsigned i = 1; i < num_clients; ++i ) {
//       auto stream = co_await a;
//       CHECK( stream.has_value() );
//     }

//     ++num_runs;
//   };

//   auto client = []( fiona::executor ex,
//                     std::uint16_t port ) -> fiona::task<void> {
//     auto client_result = co_await fiona::tcp::client::make( ex );
//     auto& client = client_result.value();
//     client.timeout( std::chrono::seconds( 3 ) );
//     auto ec = co_await client.async_connect( localhost, port );
//     (void)ec;
//     // CHECK( !ec );
//     // if ( ec ) {
//     //   throw ec;
//     // }
//     ++num_runs;
//   };

//   ex.post( server( std::move( acceptor ), ex ) );
//   for ( unsigned i = 0; i < num_clients; ++i ) {
//     ex.post( client( ex, port ) );
//   }

//   ioc.run();

//   CHECK( num_runs == num_clients + 1 );
// }

TEST_CASE( "client connection refused" ) {
  num_runs = 0;

  auto client = []( fiona::executor ex ) -> fiona::task<void> {
    auto maybe_ec = co_await fiona::tcp::client::async_connect(
        ex, localhost, 3301, std::chrono::seconds( 2 ) );

    auto ec = maybe_ec.error();
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
    // use one of the IP addresses from the test networks:
    // 192.0.2.0/24
    // https://en.wikipedia.org/wiki/Internet_Protocol_version_4#Special-use_addresses
    auto maybe_ec = co_await fiona::tcp::client::async_connect(
        ex, in_addr{ .s_addr = 0xc0'00'02'00 }, 3301,
        std::chrono::seconds( 2 ) );
    auto ec = maybe_ec.error();

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
    auto maybe_client = co_await fiona::tcp::client::async_connect(
        ex, localhost, port, std::chrono::seconds( 3 ) );

    auto& client = maybe_client.value();

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

TEST_CASE( "fd reuse" ) {
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

  fiona::tcp::acceptor acceptor( ex, localhost, 0 );
  auto const port = acceptor.port();

  auto server = []( fiona::tcp::acceptor acceptor ) -> fiona::task<void> {
    std::random_device rd;
    std::mt19937 g( rd() );

    std::uint32_t num_accepted = 0;

    std::vector<fiona::tcp::stream> sessions;

    while ( num_accepted < total_connections ) {
      auto stream = co_await acceptor.async_accept();
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

      sessions.push_back( std::move( stream.value() ) );
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
      auto maybe_client =
          co_await fiona::tcp::client::async_connect( ex, localhost, port );

      auto& client = maybe_client.value();

      char const msg[] = "hello, world!";
      auto result = co_await client.async_write( msg, std::size( msg ) );
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

// things to test:
// * removing buffer groups during async ops
// * buffer exhausting
// * recv timeouts
// * exceptions being thrown at random times
// * update accept() to be more result-oriented instead of just `i32`
