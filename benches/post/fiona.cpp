#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/task.hpp>
#include <fiona/time.hpp>

#include <thread>

#include "common.hpp"

using namespace std::chrono_literals;

void
fiona_post_bench()
{
  fiona::io_context ioc;
  std::vector<std::jthread> threads;

  static int num_runs = 0;

  auto ex = ioc.get_executor();
  fiona::timer timer( ex );

  auto task = []( fiona::timer& timer ) -> fiona::task<void> {
    ++num_runs;
    if ( num_runs == total_runs ) {
      co_await timer.async_cancel();
    }
    co_return;
  };

  ex.spawn( []( fiona::timer timer ) -> fiona::task<void> {
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
