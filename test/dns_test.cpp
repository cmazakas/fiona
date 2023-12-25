#include "helpers.hpp"

#include <fiona/dns.hpp>
#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>

#include <arpa/inet.h>

static int num_runs = 0;

TEST_CASE( "dns_test - fetching the list of remote IP addresses" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( FIONA_TASK( fiona::executor ex ) {
    fiona::dns_resolver resolver( ex );

    auto mentrylist =
        co_await resolver.async_resolve( "www.bing.com", "https" );

    CHECK( mentrylist.has_value() );

    auto& entrylist = mentrylist.value();

    int resolved_addrs = 0;

    for ( auto const* pai = entrylist.data(); pai; pai = pai->ai_next ) {
      if ( ( pai->ai_family == AF_INET ) || ( pai->ai_family == AF_INET6 ) ) {
        ++resolved_addrs;
      }
    }

    CHECK( resolved_addrs > 0 );
    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}

TEST_CASE( "dns_test - fetching a non-existent entry" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( FIONA_TASK( fiona::executor ex ) {
    fiona::dns_resolver resolver( ex );

    auto mentrylist =
        co_await resolver.async_resolve( "www.lmaobro.rawr", "https" );

    CHECK( mentrylist.has_error() );
    CHECK( mentrylist.error().value() == EAI_NONAME );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}
