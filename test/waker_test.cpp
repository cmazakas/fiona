#include "helpers.hpp"

#include <fiona/io_context.hpp>

#include <atomic>
#include <thread>
#include <vector>

static std::atomic_int num_runs = 0;

struct custom_awaitable {
  std::shared_ptr<std::vector<int>> nums;
  std::thread t;
  fiona::executor ex;
  std::shared_ptr<std::mutex> m;
  bool should_detach = false;

  custom_awaitable( fiona::executor ex_ ) : ex{ ex_ } {
    nums = std::make_shared<std::vector<int>>();
    m = std::make_shared<std::mutex>();
  }

  ~custom_awaitable() {
    if ( should_detach ) {
      t.detach();
    } else {
      t.join();
    }
  }

  bool await_ready() const noexcept { return false; }

  void await_suspend( std::coroutine_handle<> h ) {
    auto waker = ex.make_waker( h );

    t = std::thread( [nums = this->nums, m = this->m,
                      should_detach = this->should_detach, waker]() mutable {
      std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
      {
        std::lock_guard lg{ *m };
        *nums = std::vector{ 1, 2, 3, 4 };
      }
      ++num_runs;

      try {
        waker.wake();
      } catch ( fiona::error_code const& ec ) {
        CHECK( should_detach );
        CHECK( ec == fiona::error_code::from_errno( EINVAL ) );
        return;
      }

      CHECK( !should_detach );
    } );
  }

  std::vector<int> await_resume() {
    std::lock_guard lg{ *m };
    return std::move( *nums );
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

TEST_CASE( "pipe_waker outlives the io_context" ) {
  num_runs = 0;

  {
    fiona::io_context ioc;
    auto ex = ioc.get_executor();

    ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
      ++num_runs;
      auto a = custom_awaitable( ex );
      a.should_detach = true;
      co_await a;
      // we should never hit this
      CHECK( false );
    }( ex ) );

    ioc.post( []( fiona::executor ex ) -> fiona::task<void> {
      (void)ex;
      ++num_runs;
      throw "a random error occurred!!!!!";
      co_return;
    }( ex ) );

    CHECK_THROWS( ioc.run() );
  }

  std::this_thread::sleep_for( std::chrono::milliseconds( 750 ) );
  CHECK( num_runs == 3 );
}