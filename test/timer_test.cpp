// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "helpers.hpp"

#include <fiona/time.hpp>

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>

#include <boost/config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#if BOOST_CLANG
#pragma clang diagnostic ignored "-Wglobal-constructors"
#pragma clang diagnostic ignored "-Wunreachable-code"
#endif

std::chrono::milliseconds get_sleep_duration();

static int num_runs = 0;

struct seeder
{
  seeder( unsigned seed ) { std::srand( seed ); }
};

static seeder initialize_seed( 4122023 );

std::chrono::milliseconds
get_sleep_duration()
{
  return std::chrono::milliseconds{ 200 + ( std::rand() % 1000 ) };
}

namespace {

template <class Rep, class Period>
fiona::task<void>
sleep_coro( fiona::timer timer, std::chrono::duration<Rep, Period> d )
{
  {
    duration_guard guard( d );
    auto r = co_await timer.async_wait( d );
    CHECK( r.has_value() );
  }

  ++num_runs;
  co_return;
}

fiona::task<void>
nested_sleep_coro( fiona::executor ex )
{
  auto timer = fiona::timer( ex );

  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    auto r = co_await timer.async_wait( d );
    CHECK( r );
  }

  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    co_await sleep_coro( timer, d );
  }

  ++num_runs;
  co_return;
}

fiona::task<void>
nested_sleep_coro_late_return( fiona::executor ex )
{
  auto timer = fiona::timer( ex );
  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    co_await sleep_coro( timer, d );
  }

  {
    auto d = get_sleep_duration();
    duration_guard guard( d );
    auto r = co_await timer.async_wait( d );
    CHECK( r );
  }

  ++num_runs;
}

fiona::task<void>
empty_coroutine( fiona::executor )
{
  ++num_runs;
  co_return;
}

fiona::task<void>
nested_post_timer( fiona::executor ex )
{
  ex.spawn( nested_sleep_coro( ex ) );
  ex.spawn( nested_sleep_coro_late_return( ex ) );

  ++num_runs;
  co_return;
}

fiona::task<void>
recursion_test( fiona::executor ex, int n )
{
  if ( n == 0 ) {
    ++num_runs;
    co_return;
  }

  co_await recursion_test( ex, n - 1 );
}

fiona::task<void>
return_value_test()
{
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

TEST_CASE( "timer_test - single sleep" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  auto timer = fiona::timer( ex );
  ioc.spawn( sleep_coro( timer, get_sleep_duration() ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 );
}

TEST_CASE( "timer_test - nested coroutine invocation" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( nested_sleep_coro( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 2 );
}

TEST_CASE( "timer_test - nested coroutine invocation (part 2)" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( nested_sleep_coro_late_return( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 2 );
}

TEST_CASE( "timer_test - empty coroutine" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( empty_coroutine( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 );
}

TEST_CASE( "timer_test - multiple concurrent tasks" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  auto timer = fiona::timer( ex );
  ioc.spawn( sleep_coro( timer, get_sleep_duration() ) );
  ioc.spawn( nested_sleep_coro( ex ) );
  ioc.spawn( nested_sleep_coro_late_return( ex ) );
  ioc.spawn( empty_coroutine( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 + 2 + 2 + 1 );
}

TEST_CASE( "timer_test - nested post() invocation" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( nested_post_timer( ex ) );
  ioc.run();
  CHECK_EQ( num_runs, 1 + 2 + 2 );
}

#if defined( RUN_SYMMETRIC_TRANSFER_TESTS )

TEST_CASE( "timer_test - recursion test" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  for ( int i = 0; i < 10; ++i ) {
    ioc.spawn( recursion_test( ex, 1'000'000 ) );
  }
  ioc.run();
  CHECK_EQ( num_runs, 10 );
}

#endif

TEST_CASE( "timer_test - mild stress test" )
{
  num_runs = 0;
  fiona::io_context_params params;
  params.sq_entries = 4096;
  fiona::io_context ioc( params );
  auto ex = ioc.get_executor();

  std::vector<fiona::timer> timers;
  timers.reserve( 1000 );

  for ( int i = 0; i < 1000; ++i ) {
    auto timer = fiona::timer( ex );
    timers.push_back( std::move( timer ) );
    ioc.spawn( sleep_coro( timers.back(), get_sleep_duration() ) );
  }
  ioc.run();
  CHECK_EQ( num_runs, 1000 );
}

TEST_CASE( "timer_test - coroutine return test" )
{
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( return_value_test() );
  ioc.run();
  CHECK_EQ( num_runs, 1 );
}

TEST_CASE( "timer_test - reusable timer" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  auto timer_op = []( fiona::executor ex ) -> fiona::task<void> {
    fiona::timer timer( ex );
    for ( int i = 0; i < 10; ++i ) {
      auto d = get_sleep_duration() / 2;
      duration_guard dg( d );
      auto r = co_await timer.async_wait( d );
      CHECK( r.has_value() );
    }
    ++num_runs;
  };

  for ( int i = 0; i < 1000; ++i ) {
    ioc.spawn( timer_op( ex ) );
  }

  ioc.run();
  CHECK_EQ( num_runs, 1000 );
}

TEST_CASE( "timer_test - async cancellation" )
{
  num_runs = 0;

  fiona::io_context ioc;

  ioc.spawn( []( fiona::executor ex ) -> fiona::task<void> {
    fiona::timer timer( ex );

    auto h = fiona::spawn( ex, []( fiona::timer timer ) -> fiona::task<void> {
      fiona::timer t2( timer.get_executor() );
      co_await t2.async_wait( std::chrono::milliseconds( 250 ) );
      auto r = co_await timer.async_cancel();
      CHECK( r.has_value() );
      ++num_runs;
    }( timer ) );

    auto r = co_await timer.async_wait( std::chrono::milliseconds( 1000 ) );
    CHECK( r.has_error() );
    CHECK( r.error() == fiona::error_code::from_errno( ECANCELED ) );
    co_await h;
    ++num_runs;
  }( ioc.get_executor() ) );

  ioc.run();
  CHECK( num_runs == 2 );
}

TEST_CASE( "timer_test - cancellation canceled on drop" )
{
  num_runs = 0;

  fiona::io_context ioc;

  ioc.spawn( []( fiona::executor ex ) -> fiona::task<void> {
    fiona::timer timer( ex );
    ++num_runs;
    auto r = co_await timer.async_cancel();
    (void)r;
    CHECK( false );
  }( ioc.get_executor() ) );

  ioc.spawn( []() -> fiona::task<void> {
    ++num_runs;
    throw 1234;
    co_return;
  }() );

  CHECK_THROWS( ioc.run() );
  CHECK( num_runs == 2 );
}
