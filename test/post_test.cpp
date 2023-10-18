#include "helpers.hpp"

#include <fiona/error.hpp>
#include <fiona/io_context.hpp>
#include <fiona/time.hpp>

#include <boost/config.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

static std::atomic_int num_runs = 0;

inline constexpr std::chrono::milliseconds sleep_dur( 25 );

fiona::task<std::string>
make_string( fiona::executor ex ) {
  ++num_runs;
  co_await fiona::sleep_for( ex, sleep_dur );
  co_return std::string( "hello world! this should hopefully break sbo and "
                         "force a dynamic allocation" );
}

fiona::task<std::unique_ptr<int>>
make_int_pointer( fiona::executor ex ) {
  ++num_runs;
  co_await fiona::sleep_for( ex, sleep_dur );
  auto p = std::make_unique<int>( 1337 );
  co_return std::move( p );
}

fiona::task<int>
throw_exception( fiona::executor ex ) {
  ++num_runs;
  co_await fiona::sleep_for( ex, sleep_dur );
  throw "random error";

  co_return 1337;
}

TEST_CASE( "post_test - awaiting a sibling coro" ) {
  num_runs = 0;

  fiona::io_context ioc;

  auto ex = ioc.get_executor();
  ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
    {
      duration_guard dg( 2 * sleep_dur );

      auto str = co_await fiona::post( ex, make_string( ex ) );

      CHECK( str == "hello world! this should hopefully break sbo and "
                    "force a dynamic allocation" );

      CHECK_THROWS( co_await fiona::post( ex, throw_exception( ex ) ) );
    }

    {
      duration_guard dg( sleep_dur );
      auto h1 = fiona::post( ex, make_string( ex ) );
      auto h2 = fiona::post( ex, throw_exception( ex ) );

      auto str = co_await h1;
      CHECK_THROWS( co_await h2 );

      CHECK( str == "hello world! this should hopefully break sbo and "
                    "force a dynamic allocation" );
    }

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();

  // 1 for each child, 2x + parent coro
  CHECK( num_runs == 2 * 2 + 1 );
}

TEST_CASE( "post_test - ignoring exceptions" ) {
  num_runs = 0;
  fiona::io_context ioc;

  auto ex = ioc.get_executor();
  fiona::post( ex, []( fiona::executor ex ) -> fiona::task<void> {
    auto h = fiona::post( ex, throw_exception( ex ) );
    (void)h;
    ++num_runs;
    co_return;
  }( ex ) );

  duration_guard dg( sleep_dur );
  CHECK_THROWS( ioc.run() );
  CHECK( num_runs == 2 );
}

TEST_CASE( "post_test - posting a move-only type" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  fiona::post( ex, []( fiona::executor ex ) -> fiona::task<void> {
    duration_guard dg( sleep_dur );
    auto h = fiona::post( ex, make_int_pointer( ex ) );

    {
      auto p = co_await make_int_pointer( ex );
      CHECK( *p == 1337 );
    }

    {
      auto p = co_await h;
      CHECK( *p == 1337 );
    }

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 3 );
}

TEST_CASE( "post_test - void returning function" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ex.post( []( fiona::executor ex ) -> fiona::task<void> {
    co_await fiona::post( ex, []( fiona::executor ex ) -> fiona::task<void> {
      co_await fiona::sleep_for( ex, std::chrono::milliseconds( 500 ) );
      ++num_runs;
    }( ex ) );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

#if defined( RUN_SYMMETRIC_TRANSFER_TESTS )

fiona::task<void>
empty_task() {
  co_return;
}

fiona::task<void>
symmetric_transfer_test() {
  for ( int i = 0; i < 1'000'000; ++i ) {
    co_await empty_task();
  }
  ++num_runs;
  co_return;
}

TEST_CASE( "post_test - symmetric transfer" ) {
  num_runs = 0;

  fiona::io_context ioc;
  ioc.post( symmetric_transfer_test() );
  ioc.run();
  CHECK( num_runs == 1 );
}

#endif

TEST_CASE( "post_test - destruction on a separate thread" ) {
  num_runs = 0;

  std::thread t;

  {
    fiona::io_context ioc;
    auto ex = ioc.get_executor();

    ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
      auto ec =
          co_await fiona::sleep_for( ex, std::chrono::milliseconds( 250 ) );
      CHECK( !ec );
      ++num_runs;
      co_return;
    }( ex ) );

    ioc.run();

    t = std::thread( [&ioc] {
      auto ex = ioc.get_executor();
      (void)ex;
      std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
      ++num_runs;
    } );

    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
  }

  t.join();
  CHECK( num_runs == 2 );
}
