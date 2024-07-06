#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/task.hpp>
#include <fiona/time.hpp>

int
main()
{
  fiona::io_context ioc;
  fiona::timer timer( ioc.get_executor() );

  ioc.post( []( fiona::timer timer ) -> fiona::task<void>
  {
    auto mok = co_await timer.async_wait( std::chrono::milliseconds{ 250 } );
    (void)mok.value();
    co_return;
  }( timer ) );

  ioc.run();
}
