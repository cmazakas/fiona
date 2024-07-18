#include <fiona/file.hpp>
#include <fiona/io_context.hpp>

#include "helpers.hpp"

static int num_runs = 0;

TEST_CASE( "creating a new file" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ex.spawn( []( fiona::executor ex ) -> fiona::task<void>
  {
    fiona::file file( ex );

    auto m_ok = co_await file.async_open( "/tmp", O_TMPFILE | O_RDWR );
    CHECK( m_ok.has_value() );
    (void)m_ok.value();

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}
