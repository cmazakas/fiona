#include "common.hpp"

#if defined( BOOST_GCC ) && defined( __SANITIZE_THREAD__ )

#include <iostream>

void
asio_timer_bench() {
  std::cout << "Asio does not support building with tsan and gcc!" << std::endl;
  CHECK( false );
}

#else

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

void
asio_timer_bench() {
  static std::atomic_int anums = 0;

  boost::asio::io_context ioc( 1 );

  for ( int i = 0; i < 10'000; ++i ) {
    boost::asio::co_spawn(
        ioc.get_executor(),
        []( boost::asio::any_io_executor ex ) -> boost::asio::awaitable<void> {
          boost::asio::steady_timer timer( ex );
          for ( int i = 0; i < 10'000; ++i ) {
            timer.expires_after( 1ms );
            co_await timer.async_wait( boost::asio::use_awaitable );
            anums.fetch_add( 1, std::memory_order_relaxed );
          }
          co_return;
        }( ioc.get_executor() ),
        boost::asio::detached );
  }
  ioc.run();

  CHECK( anums == 10'000 * 10'000 );
}
#endif
