#include "helpers.hpp"

#include <fiona/error.hpp>
#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/time.hpp>

#include <boost/config.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#if BOOST_CLANG
#pragma clang diagnostic ignored "-Wunreachable-code"
#endif

static std::atomic_int num_runs = 0;

inline constexpr std::chrono::milliseconds sleep_dur( 25 );

namespace {

fiona::task<std::string>
make_string( fiona::executor ex )
{
  ++num_runs;
  fiona::timer timer( ex );
  auto r = co_await timer.async_wait( sleep_dur );
  CHECK( r.has_value() );
  co_return std::string( "hello world! this should hopefully break sbo and "
                         "force a dynamic allocation" );
}

fiona::task<std::unique_ptr<int>>
make_int_pointer( fiona::executor ex )
{
  ++num_runs;
  fiona::timer timer( ex );
  auto r = co_await timer.async_wait( sleep_dur );
  CHECK( r.has_value() );

  auto p = std::make_unique<int>( 1337 );
  co_return std::move( p );
}

fiona::task<int>
throw_exception( fiona::executor ex )
{
  ++num_runs;
  fiona::timer timer( ex );
  auto r = co_await timer.async_wait( sleep_dur );
  CHECK( r.has_value() );

  throw "random error";

  co_return 1337;
}

} // namespace

TEST_CASE( "awaiting a sibling coro" )
{
  num_runs = 0;

  fiona::io_context ioc;

  auto ex = ioc.get_executor();
  ex.spawn( []( fiona::executor ex ) -> fiona::task<void> {
    {
      duration_guard dg( 2 * sleep_dur );

      auto str = co_await fiona::spawn( ex, make_string( ex ) );

      CHECK( str == "hello world! this should hopefully break sbo and "
                    "force a dynamic allocation" );

      CHECK_THROWS( co_await fiona::spawn( ex, throw_exception( ex ) ) );
    }

    {
      duration_guard dg( sleep_dur );
      auto h1 = fiona::spawn( ex, make_string( ex ) );
      auto h2 = fiona::spawn( ex, throw_exception( ex ) );

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

TEST_CASE( "ignoring exceptions" )
{
  // test the following:
  // * coroutine destroyed without a post_awaitable object in its frame
  // * coroutine destroyed with a post_awaitable in its frame
  // * destroy a nested coroutine for each of the variants above

  num_runs = 0;
  fiona::io_context ioc;

  auto ex = ioc.get_executor();
  ex.spawn( FIONA_TASK( fiona::executor ex ) {
    ++num_runs;
    fiona::timer timer( ex );
    co_await timer.async_wait( 500ms );
    CHECK( false );
    co_return;
  }( ex ) );

  ex.spawn( FIONA_TASK( fiona::executor ex ) {
    auto h = fiona::spawn( ex, FIONA_TASK( fiona::executor ex ) {
      ++num_runs;
      fiona::timer timer( ex );
      co_await timer.async_wait( 500ms );
      CHECK( false );
      co_return;
    }( ex ) );

    (void)h;

    ++num_runs;
    fiona::timer timer( ex );
    co_await timer.async_wait( 500ms );
    CHECK( false );
  }( ex ) );

  ex.spawn( FIONA_TASK( fiona::executor ex ) {
    auto inner_task = FIONA_TASK( fiona::executor ex )
    {
      ++num_runs;
      fiona::timer timer( ex );
      co_await timer.async_wait( 500ms );
      CHECK( false );
      co_return;
    }
    ( ex );

    co_await inner_task;
    CHECK( false );
    co_return;
  }( ex ) );

  ex.spawn( FIONA_TASK( fiona::executor ex ) {
    auto inner_task = FIONA_TASK( fiona::executor ex )
    {
      auto h = fiona::spawn( ex, FIONA_TASK( fiona::executor ex ) {
        ++num_runs;
        fiona::timer timer( ex );
        co_await timer.async_wait( 500ms );
        CHECK( false );
      }( ex ) );

      (void)h;

      fiona::spawn( ex, FIONA_TASK() {
        throw "a random error";
        co_return;
      }() );

      ++num_runs;
      fiona::timer timer( ex );
      co_await timer.async_wait( 500ms );
      CHECK( false );
      co_return;
    }
    ( ex );

    co_await inner_task;
    CHECK( false );
    co_return;
  }( ex ) );

  CHECK_THROWS( ioc.run() );
  CHECK( num_runs == 6 );
}

TEST_CASE( "posting a move-only type" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  fiona::spawn( ex, []( fiona::executor ex ) -> fiona::task<void> {
    duration_guard dg( sleep_dur );
    auto h = fiona::spawn( ex, make_int_pointer( ex ) );

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

TEST_CASE( "void returning function" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ex.spawn( []( fiona::executor ex ) -> fiona::task<void> {
    co_await fiona::spawn( ex, []( fiona::executor ex ) -> fiona::task<void> {
      fiona::timer timer( ex );
      auto r = co_await timer.async_wait( std::chrono::milliseconds( 500 ) );
      CHECK( r.has_value() );

      ++num_runs;
    }( ex ) );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();

  CHECK( num_runs == 2 );
}

#if defined( RUN_SYMMETRIC_TRANSFER_TESTS )

namespace {

fiona::task<void>
empty_task()
{
  co_return;
}

fiona::task<void>
symmetric_transfer_test()
{
  for ( int i = 0; i < 1'000'000; ++i ) {
    co_await empty_task();
  }
  ++num_runs;
  co_return;
}

} // namespace

TEST_CASE( "symmetric transfer" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ex.spawn( symmetric_transfer_test() );
  ioc.run();
  CHECK( num_runs == 1 );
}

#endif

TEST_CASE( "destruction on a separate thread" )
{
  num_runs = 0;

  std::thread t;

  {
    fiona::io_context ioc;
    auto ex = ioc.get_executor();

    ex.spawn( []( fiona::executor ex ) -> fiona::task<void> {
      fiona::timer timer( ex );
      auto r = co_await timer.async_wait( std::chrono::milliseconds( 250 ) );
      CHECK( r.has_value() );

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

TEST_CASE( "inter-thread posting" )
{
  fiona::io_context ioc;
  std::vector<std::jthread> threads;

  int const num_threads = 8;
  int const num_tasks = 25'000;
  int const total_runs = num_threads * num_tasks;
  static int num_runs = 0;

  auto ex = ioc.get_executor();
  fiona::timer timer( ex );

  auto task = []( fiona::timer& timer ) -> fiona::task<void> {
    fiona::timer t( timer.get_executor() );
    co_await t.async_wait( 250ms );

    ++num_runs;
    if ( num_runs == total_runs ) {
      co_await timer.async_cancel();
    }

    co_return;
  };

  ex.spawn( FIONA_TASK( fiona::timer timer ) {
    co_await timer.async_wait( 100s );
    co_return;
  }( timer ) );

  for ( int i = 0; i < num_threads; ++i ) {
    threads.emplace_back( [ex, task, &timer] {
      for ( int j = 0; j < num_tasks; ++j ) {
        ex.post( task( timer ) );
      }
    } );
  }

  ioc.run();
  CHECK( num_runs == num_threads * num_tasks );
}
