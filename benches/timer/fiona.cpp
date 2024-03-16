#include "common.hpp"

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/time.hpp>

#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

void
fiona_timer_bench() {
  static std::atomic_int anums = 0;

  fiona::io_context_params params;
  params.cq_entries = 16 * 1024;
  params.sq_entries = 16 * 1024;

  fiona::io_context ioc( params );

  for ( int i = 0; i < 10'000; ++i ) {
    ioc.spawn( []( fiona::executor ex ) -> fiona::task<void> {
      fiona::timer timer( ex );
      for ( int i = 0; i < 10'000; ++i ) {
        auto mokay = co_await timer.async_wait( 1ms );
        CHECK( mokay.has_value() );
        anums.fetch_add( 1, std::memory_order_relaxed );
      }
      co_return;
    }( ioc.get_executor() ) );
  }
  ioc.run();

  CHECK( anums == 10'000 * 10'000 );
}
