#include "helpers.hpp"

#include <fiona/io_context.hpp>

#include <thread>
#include <vector>

static int num_runs = 0;

struct custom_awaitable {
  std::vector<int> nums;
  std::thread t;
  fiona::executor ex;
  std::mutex m;

  custom_awaitable( fiona::executor ex_ ) : ex{ ex_ } {}
  ~custom_awaitable() { t.join(); }

  bool await_ready() const noexcept { return false; }

  void await_suspend( std::coroutine_handle<> h ) {
    auto waker = ex.make_waker( h );

    t = std::thread( [this, waker]() mutable {
      std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
      {
        std::lock_guard lg{ m };
        nums = std::vector{ 1, 2, 3, 4 };
      }
      ++num_runs;
      waker.wake();
    } );
  }

  std::vector<int> await_resume() {
    std::lock_guard lg{ m };
    return std::move( nums );
  }
};

TEST_CASE( "waiting a simple future" ) {

  num_runs = 0;

  fiona::io_context ioc;
  ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
    duration_guard dg( std::chrono::milliseconds( 500 ) );
    auto nums = co_await custom_awaitable( ex );
    CHECK( nums == std::vector{ 1, 2, 3, 4 } );
    ++num_runs;
    co_return;
  }( ioc.get_executor() ) );
  ioc.run();

  CHECK( num_runs == 2 );
}