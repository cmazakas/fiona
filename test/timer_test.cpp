#include "helpers.hpp"

#include <fiona/time.hpp>

#include <fiona/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <iostream>

static int num_runs = 0;

struct seeder {
  seeder( unsigned seed ) { std::srand( seed ); }
};

seeder initialize_seed( 4122023 );

std::chrono::milliseconds
get_sleep_duration() {
  return std::chrono::milliseconds{ 200 + ( std::rand() % 1000 ) };
}

namespace {

template <class Rep, class Period>
fiona::task<void>
sleep_coro( fiona::executor ex, std::chrono::duration<Rep, Period> d ) {
  {
    duration_guard guard( d );
    auto ec = co_await sleep_for( ex, d );
    CHECK( !ec );
  }

  ++num_runs;
  co_return;
}

fiona::task<void>
nested_sleep_coro( fiona::executor ex ) {
  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    auto ec = co_await sleep_for( ex, d );
    CHECK( !ec );
  }

  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    co_await sleep_coro( ex, d );
  }

  ++num_runs;
  co_return;
}

fiona::task<void>
nested_sleep_coro_late_return( fiona::executor ex ) {
  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    co_await sleep_coro( ex, d );
  }

  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    auto ec = co_await sleep_for( ex, d );
    CHECK( !ec );
  }

  ++num_runs;
}

fiona::task<void>
empty_coroutine( fiona::executor ) {
  ++num_runs;
  co_return;
}

fiona::task<void>
nested_post_timer( fiona::executor ex ) {
  ex.post( nested_sleep_coro( ex ) );
  ex.post( nested_sleep_coro_late_return( ex ) );

  ++num_runs;
  co_return;
}

fiona::task<void>
recursion_test( fiona::executor ex, int n ) {
  if ( n == 0 ) {
    ++num_runs;
    co_return;
  }

  std::chrono::microseconds d( 500 );
  auto ec = co_await sleep_for( ex, d );
  CHECK( !ec );

  co_await recursion_test( ex, n - 1 );
}

fiona::task<void>
return_value_test() {
  auto f = []() -> fiona::task<std::vector<int>> {
    co_return std::vector<int>{ 1, 2, 3, 4 };
  };

  auto vec = co_await f();
  CHECK_EQ( vec.size(), 4u );

  auto throwing = []() -> fiona::task<int> {
    throw std::logic_error( "rawr" );
    co_return 1337;
  };

  CHECK_THROWS( co_await throwing() );

  ++num_runs;
}

} // namespace

TEST_CASE( "single sleep" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( sleep_coro( ex, get_sleep_duration() ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 );
}

TEST_CASE( "nested coroutine invocation" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( nested_sleep_coro( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 2 );
}

TEST_CASE( "nested coroutine invocation (part 2)" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( nested_sleep_coro_late_return( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 2 );
}

TEST_CASE( "empty coroutine" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( empty_coroutine( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 );
}

TEST_CASE( "multiple concurrent tasks" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( sleep_coro( ex, get_sleep_duration() ) );
  ioc.post( nested_sleep_coro( ex ) );
  ioc.post( nested_sleep_coro_late_return( ex ) );
  ioc.post( empty_coroutine( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 + 2 + 2 + 1 );
}

TEST_CASE( "nested post() invocation" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( nested_post_timer( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 + 2 + 2 );
}

TEST_CASE( "recursion test" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  for ( int i = 0; i < 10; ++i ) {
    ioc.post( recursion_test( ex, 2000 ) );
  }
  ioc.run();
  CHECK_EQ( num_runs, 10 );
}

TEST_CASE( "mild stress test" ) {
  num_runs = 0;
  fiona::io_context_params params;
  params.sq_entries = 4096;
  fiona::io_context ioc( params );
  auto ex = ioc.get_executor();
  for ( int i = 0; i < 1000; ++i ) {
    ioc.post( sleep_coro( ex, get_sleep_duration() ) );
  }
  ioc.run();
  CHECK_EQ( num_runs, 1000 );
}

TEST_CASE( "coroutine return test" ) {
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( return_value_test() );
  ioc.run();
  CHECK_EQ( num_runs, 1 );
}
