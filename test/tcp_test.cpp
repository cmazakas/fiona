#include <fiona/io_context.hpp>
#include <fiona/tcp.hpp>

#include <boost/core/lightweight_test.hpp>

namespace {

void
acceptor_test() {
  constexpr std::uint32_t localhost = 0x7f000001;
  constexpr std::uint16_t port = 3030;
  constexpr int num_clients = 1;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();

  ioc.post( ( []( fiona::executor ex ) -> fiona::task<void> {
    fiona::tcp::acceptor acceptor( ex, localhost, port );
    auto a = acceptor.async_accept();

    for ( int num_accepted = 0; num_accepted < num_clients; ++num_accepted ) {
      auto fd = co_await a;
      close( fd );
    }

    co_return;
  } )( ex ) );

  ioc.post( ( []( fiona::executor ex ) -> fiona::task<void> {
    fiona::tcp::client client( ex );

    auto ec = co_await client.async_connect( localhost, port );
    BOOST_TEST( !ec );
  } )( ex ) );

  ioc.run();
}

} // namespace

int
main() {
  acceptor_test();
  return boost::report_errors();
}
