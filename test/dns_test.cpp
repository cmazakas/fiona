#include "helpers.hpp"               // for FIONA_TASK

#include <fiona/borrowed_buffer.hpp> // for borrowed_buffer
#include <fiona/dns.hpp>             // for dns_entry_list, dns_awaitable, dns_resolver
#include <fiona/error.hpp>           // for result, error_code
#include <fiona/executor.hpp>        // for executor
#include <fiona/io_context.hpp>      // for io_context
#include <fiona/tcp.hpp>             // for client, connect_awaitable, recv_awaitable, send_awaitable, stream_cl...

#include <cstddef>                   // for size_t
#include <iostream>                  // for char_traits, basic_ostream, endl, cout
#include <span>                      // for span

#include <netdb.h>                   // for addrinfo, EAI_NONAME
#include <string_view>               // for operator<<, string_view
#include <sys/socket.h>              // for AF_INET, AF_INET6

static int num_runs = 0;

TEST_CASE( "dns_test - fetching the list of remote IP addresses" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( FIONA_TASK( fiona::executor ex ) {
    fiona::dns_resolver resolver( ex );

    auto mentrylist = co_await resolver.async_resolve( "www.bing.com", "https" );

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

    auto mentrylist = co_await resolver.async_resolve( "www.lmaobro.rawr", "https" );

    CHECK( mentrylist.has_error() );
    CHECK( mentrylist.error().value() == EAI_NONAME );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}

TEST_CASE( "dns_test - connecting a client" ) {
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post( FIONA_TASK( fiona::executor ex ) {
    fiona::dns_resolver resolver( ex );

    auto mentrylist = co_await resolver.async_resolve( "www.google.com", "http" );

    CHECK( mentrylist.has_value() );

    auto& entrylist = mentrylist.value();

    sockaddr const* remote_addr = nullptr;
    for ( auto const* pai = entrylist.data(); pai; pai = pai->ai_next ) {
      if ( ( pai->ai_family == AF_INET ) || ( pai->ai_family == AF_INET6 ) ) {
        remote_addr = pai->ai_addr;
        break;
      }
    }

    fiona::tcp::client client( ex );
    client.timeout( 3s );

    auto mok = co_await client.async_connect( remote_addr );
    CHECK( mok.has_value() );

    auto req = std::string_view( "GET / HTTP/1.1\r\n"
                                 "Host: www.google.com\r\n"
                                 "Connection: close\r\n"
                                 "\r\n" );

    co_await client.async_send( req );

    ex.register_buffer_sequence( 1024, 128, 0 );
    auto rx = client.get_receiver( 0 );

    std::size_t num_read = 0;
    while ( true ) {
      auto mborrowed_buf = co_await rx.async_recv();
      if ( mborrowed_buf.has_error() ) {
        break;
      }

      auto& borrowed_buf = mborrowed_buf.value();
      if ( borrowed_buf.readable_bytes().empty() ) {
        break;
      }

      num_read += borrowed_buf.readable_bytes().size();

      std::cout << borrowed_buf.as_str() << std::endl;
    }

    co_await client.async_close();

    CHECK( num_read > 0 );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}
