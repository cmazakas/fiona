#include "helpers.hpp"

#include <fiona/io_context.hpp>

#include <thread>
#include <vector>

static int num_runs = 0;

struct custom_awaitable {
  fiona::executor ex;
  std::thread t;

  custom_awaitable( fiona::executor ex_ ) : ex{ ex_ } {}
  ~custom_awaitable() { t.join(); }

  bool await_ready() const noexcept { return false; }

  void await_suspend( std::coroutine_handle<> h ) {
    auto waker = ex.make_waker( h );

    t = std::thread( [waker] {
      std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
      ++num_runs;
      waker.wake();
    } );
  }

  void await_resume() {}
};

TEST_CASE( "waiting a simple future" ) {

  num_runs = 0;

  fiona::io_context ioc;
  ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
    duration_guard dg( std::chrono::milliseconds( 500 ) );
    co_await custom_awaitable( ex );
    ++num_runs;
    co_return;
  }( ioc.get_executor() ) );
  ioc.run();

  CHECK( num_runs == 2 );
}