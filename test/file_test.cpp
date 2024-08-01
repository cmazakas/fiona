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

    std::size_t const num_bufs = 1024;
    std::size_t const buf_size = 4096;
    auto& fixed_bufs = ex.register_fixed_buffers( num_bufs, buf_size );

    auto buf = fixed_bufs.get_avail_buf();

    auto m_ok = co_await file.async_open( "/tmp", O_TMPFILE | O_RDWR );
    CHECK( m_ok.has_value() );

    std::string_view msg = "hello, world!";
    auto m_written = co_await file.async_write( msg );
    CHECK( m_written.value() == msg.size() );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}
