#include "helpers.hpp"

#include <fiona/error.hpp>
#include <fiona/io_context.hpp>
#include <fiona/sleep.hpp>

#include <memory>
#include <string>

static int num_runs = 0;

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

TEST_CASE( "awaiting a sibling coro" ) {
  num_runs = 0;

  fiona::io_context ioc;

  auto ex = ioc.get_executor();
  ioc.post( ( []( fiona::executor ex ) -> fiona::task<void> {
    {
      duration_guard dg( 2 * sleep_dur );

      auto str = co_await ex.post( make_string( ex ) );

      CHECK( str == "hello world! this should hopefully break sbo and "
                    "force a dynamic allocation" );

      CHECK_THROWS( co_await ex.post( throw_exception( ex ) ) );
    }

    {
      duration_guard dg( sleep_dur );
      auto h1 = ex.post( make_string( ex ) );
      auto h2 = ex.post( throw_exception( ex ) );

      auto str = co_await h1;
      CHECK_THROWS( co_await h2 );

      CHECK( str == "hello world! this should hopefully break sbo and "
                    "force a dynamic allocation" );
    }

    ++num_runs;
    co_return;
  } )( ex ) );

  ioc.run();

  // 1 for each child, 2x + parent coro
  CHECK( num_runs == 2 * 2 + 1 );
}

TEST_CASE( "ignoring exceptions" ) {
  num_runs = 0;
  fiona::io_context ioc;

  auto ex = ioc.get_executor();
  ex.post( ( []( fiona::executor ex ) -> fiona::task<void> {
    auto h = ex.post( throw_exception( ex ) );
    (void)h;
    ++num_runs;
    co_return;
  } )( ex ) );

  duration_guard dg( sleep_dur );
  CHECK_THROWS( ioc.run() );
  CHECK( num_runs == 2 );
}

TEST_CASE( "posting a move-only type" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ex.post( ( []( fiona::executor ex ) -> fiona::task<void> {
    duration_guard dg( sleep_dur );
    auto h = ex.post( make_int_pointer( ex ) );

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
  } )( ex ) );

  ioc.run();
  CHECK( num_runs == 3 );
}
