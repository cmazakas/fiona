#include "helpers.hpp"        // for FIONA_TASK

#include <fiona/dns.hpp>      // for dns_entry_list, dns_awaitable, dns_resolver
#include <fiona/error.hpp>    // for result, error_code
#include <fiona/executor.hpp> // for executor
#include <fiona/io_context.hpp> // for io_context
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp> // for client, connect_awaitable, recv_awaitable, send_awaitable, stream_cl...

#include <cstddef>      // for size_t
#include <iostream>     // for char_traits, basic_ostream, endl, cout
#include <span>         // for span

#include <netdb.h>      // for addrinfo, EAI_NONAME
#include <string_view>  // for operator<<, string_view
#include <sys/socket.h> // for AF_INET, AF_INET6

static int num_runs = 0;

TEST_CASE( "fetching the list of remote IP addresses" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( FIONA_TASK( fiona::executor ex ) {
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

TEST_CASE( "fetching a non-existent entry" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( FIONA_TASK( fiona::executor ex ) {
    fiona::dns_resolver resolver( ex );

    auto m_entrylist =
        co_await resolver.async_resolve( "www.lmaobro.rawr", "https" );

    if ( m_entrylist.has_value() ) {
      auto& entrylist = m_entrylist.value();
      for ( auto p = entrylist.data(); p; p = p->ai_next ) {
        REQUIRE( p->ai_family == AF_INET );
        auto p_ipv4 = reinterpret_cast<sockaddr_in const*>( p->ai_addr );
        // this is the AT&T DNS assist IP that gets returned
        // not everyone will have this but I do when using my laptop...
        CHECK( fiona::ip::to_string( p_ipv4 ) == "143.244.220.150" );
      }
    } else {
      CHECK( m_entrylist.has_error() );
      CHECK( m_entrylist.error().value() == EAI_NONAME );
    }

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}

TEST_CASE( "connecting a client" )
{
  num_runs = 0;

  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.spawn( FIONA_TASK( fiona::executor ex ) {
    fiona::dns_resolver resolver( ex );

    auto mentrylist =
        co_await resolver.async_resolve( "www.google.com", "http" );

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
    client.set_buffer_group( 0 );

    std::size_t num_read = 0;
    while ( true ) {
      auto mbufs = co_await client.async_recv();
      if ( mbufs.has_error() ) {
        break;
      }

      auto& bufs = mbufs.value();
      auto octets = bufs.to_bytes();
      if ( octets.empty() ) {
        break;
      }

      num_read += octets.size();
    }

    co_await client.async_close();

    CHECK( num_read > 0 );

    ++num_runs;
    co_return;
  }( ex ) );

  ioc.run();
  CHECK( num_runs == 1 );
}
