#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <thread>

#include "common.hpp"

using namespace std::chrono_literals;

namespace asio = boost::asio;

void
asio_post_bench() {
  asio::io_context ioc;
  std::vector<std::jthread> threads;

  static int num_runs = 0;

  auto ex = ioc.get_executor();
  asio::steady_timer timer( ex );

  auto task = [ptimer = &timer]() -> asio::awaitable<void> {
    ++num_runs;
    if ( num_runs == total_runs ) {
      ptimer->cancel();
    }
    co_return;
  };

  asio::co_spawn(
      ex,
      [ptimer = &timer]() mutable -> asio::awaitable<void> {
        ptimer->expires_after( 100s );
        co_await ptimer->async_wait( asio::use_awaitable );
        co_return;
      },
      asio::detached );

  for ( int i = 0; i < num_threads; ++i ) {
    threads.emplace_back( [ex, task] {
      for ( int j = 0; j < num_tasks; ++j ) {
        asio::co_spawn( ex, task, asio::detached );
      }
    } );
  }

  ioc.run();
  CHECK( num_runs == num_threads * num_tasks );
}
