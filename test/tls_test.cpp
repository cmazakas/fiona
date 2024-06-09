// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "helpers.hpp"

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>
#include <fiona/tls.hpp>

#include <botan/certstor.h>
#include <botan/credentials_manager.h>
#include <botan/data_src.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/pkix_enums.h>
#include <botan/system_rng.h>
#include <botan/tls_alert.h>
#include <botan/tls_callbacks.h>
#include <botan/tls_client.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_server_info.h>
#include <botan/tls_session_manager.h>
#include <botan/tls_session_manager_memory.h>
#include <botan/x509_key.h>
#include <botan/x509cert.h>
#include <botan/x509path.h>
#include <botan/x509self.h>

#include <catch2/generators/catch_generators_random.hpp>

#include <filesystem>
#include <memory>
#include <vector>

static int num_runs = 0;

struct tls_callbacks final : public Botan::TLS::Callbacks
{
  static_assert( std::is_same_v<std::uint8_t, unsigned char> );

  std::vector<unsigned char> send_buf_;
  std::vector<unsigned char> recv_buf_;
  bool close_notify_received_ = false;

  ~tls_callbacks() override;

  void
  tls_emit_data( std::span<std::uint8_t const> data ) override
  {
    send_buf_.insert( send_buf_.end(), data.begin(), data.end() );
  }

  void
  tls_record_received( uint64_t seq_no,
                       std::span<std::uint8_t const> data ) override
  {
    (void)seq_no;

    REQUIRE( !data.empty() );
    recv_buf_.insert( recv_buf_.end(), data.begin(), data.end() );
  }

  void
  tls_alert( Botan::TLS::Alert alert ) override
  {
    // if the alert type is a close_notify, we should start a graceful shutdown
    // of the connection, otherwise we're permitted to probably just do a
    // hard shutdown of the TCP connection
    CHECK( !alert.is_fatal() );
    CHECK( alert.type_string() == "close_notify" );
    CHECK( alert.type() == Botan::TLS::AlertType::CloseNotify );
    close_notify_received_ = true;
  }

  void
  tls_session_activated() override
  {
  }

  void
  tls_session_established(
      Botan::TLS::Session_Summary const& /* session */ ) override
  {
  }

  void
  tls_verify_cert_chain(
      std::vector<Botan::X509_Certificate> const& cert_chain,
      std::vector<std::optional<Botan::OCSP::Response>> const& ocsp_responses,
      std::vector<Botan::Certificate_Store*> const& trusted_roots,
      Botan::Usage_Type usage,
      std::string_view hostname,
      Botan::TLS::Policy const& policy ) override
  {

    Botan::Path_Validation_Restrictions restrictions(
        false, policy.minimum_signature_strength() );

    Botan::Path_Validation_Result result = Botan::x509_path_validate(
        cert_chain, restrictions, trusted_roots, hostname, usage,
        tls_current_timestamp(), 0ms, ocsp_responses );

    CHECK( result.successful_validation() );
  }
};

tls_callbacks::~tls_callbacks() {}

//------------------------------------------------------------------------------

struct tls_credentials_manager final : public Botan::Credentials_Manager
{
  struct cert_key_pair
  {
    Botan::X509_Certificate cert;
    std::shared_ptr<Botan::Private_Key> key;
  };

  std::vector<cert_key_pair> cert_key_pairs_;
  Botan::Certificate_Store_In_Memory cert_store_;

  ~tls_credentials_manager() override;

  tls_credentials_manager()
  {
    {
      Botan::X509_Certificate cert( "../../test/tls/botan/ca.crt.pem" );
      cert_store_.add_certificate( cert );
    }

    std::string key = "../../test/tls/botan/server.key.pem";
    REQUIRE( std::filesystem::exists( key ) );
    Botan::DataSource_Stream data_source( key );

    auto pkey = Botan::PKCS8::load_key( data_source );
    REQUIRE( pkey != nullptr );

    std::vector<Botan::X509_Certificate> certs;

    std::string cert_file = "../../test/tls/botan/server.crt.pem";
    REQUIRE( std::filesystem::exists( cert_file ) );

    Botan::DataSource_Stream cert_data_source( cert_file );
    Botan::X509_Certificate cert( cert_data_source );

    CHECK( cert_data_source.get_bytes_read() > 0 );
    cert_data_source.discard_next( cert_data_source.get_bytes_read() );
    CHECK( cert_data_source.end_of_data() );

    cert_key_pairs_.emplace_back(
        cert_key_pair{ std::move( cert ), std::move( pkey ) } );
  }

  std::shared_ptr<Botan::Private_Key>
  private_key_for( Botan::X509_Certificate const& cert,
                   std::string const& /* type */,
                   std::string const& /* context */ ) override
  {
    for ( auto const& [mcert, pkey] : cert_key_pairs_ ) {
      if ( cert == mcert ) {
        return pkey;
      }
    }

    return nullptr;
  }

  std::vector<Botan::X509_Certificate>
  find_cert_chain(
      std::vector<std::string> const& algos,
      std::vector<Botan::AlgorithmIdentifier> const& cert_signature_schemes,
      std::vector<Botan::X509_DN> const& acceptable_cas,
      std::string const& type,
      std::string const& hostname ) override
  {
    BOTAN_UNUSED( cert_signature_schemes );
    BOTAN_UNUSED( acceptable_cas );

    if ( type == "tls-server" ) {
      for ( auto const& [cert, pkey] : cert_key_pairs_ ) {
        auto pos = std::find( algos.begin(), algos.end(), pkey->algo_name() );
        if ( pos == algos.end() ) {
          continue;
        }

        if ( !hostname.empty() && cert.matches_dns_name( hostname ) ) {
          return { cert };
        }
      }
    }

    if ( type == "tls-client" ) {
      for ( auto const& dn : acceptable_cas ) {
        for ( auto const& cred : cert_key_pairs_ ) {
          if ( dn == cred.cert.issuer_dn() ) {
            return { cred.cert };
          }
        }
      }
    }

    return {};
  }

  std::vector<Botan::Certificate_Store*>
  trusted_certificate_authorities( std::string const& type,
                                   std::string const& context ) override
  {
    BOTAN_UNUSED( type, context );
    // return a list of certificates of CAs we trust for tls server certificates
    // ownership of the pointers remains with Credentials_Manager
    return { &cert_store_ };
  }
};

tls_credentials_manager::~tls_credentials_manager() {}

namespace {

fiona::task<void>
server( fiona::tcp::acceptor acceptor )
{
  auto prng = std::make_shared<Botan::System_RNG>();
  auto pcallbacks = std::make_shared<tls_callbacks>();
  auto psession_mgr =
      std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng );

  auto pcreds_mgr = std::make_shared<tls_credentials_manager>();
  auto ptls_policy = std::make_shared<Botan::TLS::Policy const>();

  Botan::TLS::Server tls_server( pcallbacks, psession_mgr, pcreds_mgr,
                                 ptls_policy, prng );

  CHECK( !tls_server.is_active() );
  CHECK( !tls_server.is_closed() );

  auto& send_buf = pcallbacks->send_buf_;
  auto& recv_buf = pcallbacks->recv_buf_;

  auto mstream = co_await acceptor.async_accept();
  auto& stream = mstream.value();

  auto ex = stream.get_executor();
  ex.register_buffer_sequence( 1024, 128, 0 );

  stream.set_buffer_group( 0 );
  while ( !tls_server.is_handshake_complete() ) {
    auto mbuffers = co_await stream.async_recv();
    auto& buf = mbuffers.value();

    CHECK( buf.to_bytes().size() > 0 );

    tls_server.received_data( buf.to_bytes() );

    if ( !send_buf.empty() ) {
      co_await stream.async_send( send_buf );
      send_buf.clear();
    }
  }

  CHECK( send_buf.empty() );
  CHECK( tls_server.is_active() );
  CHECK( tls_server.is_handshake_complete() );

  {
    auto mbuf = co_await stream.async_recv();
    tls_server.received_data( mbuf.value().to_bytes() );
  }

  CHECK( !recv_buf.empty() );
  auto msg = std::string_view( reinterpret_cast<char const*>( recv_buf.data() ),
                               recv_buf.size() );
  CHECK( msg == "hello, world! encryption is great!" );

  tls_server.send( "hello from the server!" );
  CHECK( !send_buf.empty() );

  auto mnbytes = co_await stream.async_send( send_buf );
  CHECK( mnbytes.value() == send_buf.size() );
  send_buf.clear();

  CHECK( !tls_server.is_closed() );
  CHECK( !tls_server.is_closed_for_writing() );
  CHECK( !tls_server.is_closed_for_reading() );
  CHECK( tls_server.is_active() );

  {
    auto mbuf = co_await stream.async_recv();
    tls_server.received_data( mbuf.value().to_bytes() );
    CHECK( !send_buf.empty() );
    CHECK( pcallbacks->close_notify_received_ );
  }

  mnbytes = co_await stream.async_send( send_buf );
  CHECK( mnbytes.value() == send_buf.size() );
  send_buf.clear();

  CHECK( tls_server.is_closed() );
  CHECK( tls_server.is_closed_for_writing() );
  CHECK( tls_server.is_closed_for_reading() );
  CHECK( !tls_server.is_active() );

  ++num_runs;
  co_return;
}

fiona::task<void>
client( fiona::executor ex, std::uint16_t const port )
{
  auto prng = std::make_shared<Botan::System_RNG>();
  auto pcallbacks = std::make_shared<tls_callbacks>();
  auto psession_mgr =
      std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng );

  auto pcreds_mgr = std::make_shared<tls_credentials_manager>();
  auto ptls_policy = std::make_shared<Botan::TLS::Policy const>();

  auto server_info = Botan::TLS::Server_Information( "localhost", 0 );

  Botan::TLS::Client tls_client( pcallbacks, psession_mgr, pcreds_mgr,
                                 ptls_policy, prng, server_info );

  auto& send_buf = pcallbacks->send_buf_;

  CHECK( !tls_client.is_active() );
  CHECK( !tls_client.is_closed() );

  fiona::tcp::client client( ex );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
  co_await client.async_connect( &addr );

  REQUIRE( !send_buf.empty() );

  co_await client.async_send( send_buf );
  send_buf.clear();

  ex.register_buffer_sequence( 1024, 128, 1 );
  client.set_buffer_group( 1 );

  while ( !tls_client.is_handshake_complete() ) {
    auto mbuf = co_await client.async_recv();
    auto& buf = mbuf.value();

    auto data = buf.to_bytes();

    tls_client.received_data( data );

    if ( !send_buf.empty() ) {
      co_await client.async_send( send_buf );
      send_buf.clear();
    }
  }

  CHECK( send_buf.empty() );
  CHECK( tls_client.is_active() );
  CHECK( tls_client.is_handshake_complete() );

  tls_client.send( "hello, world! encryption is great!" );
  CHECK( !send_buf.empty() );

  auto mnbytes = co_await client.async_send( send_buf );
  CHECK( mnbytes.value() == send_buf.size() );
  send_buf.clear();

  while ( pcallbacks->recv_buf_.empty() ) {
    auto mbuf = co_await client.async_recv();
    tls_client.received_data( mbuf.value().to_bytes() );
  }

  CHECK( !pcallbacks->recv_buf_.empty() );
  auto msg = std::string_view(
      reinterpret_cast<char const*>( pcallbacks->recv_buf_.data() ),
      pcallbacks->recv_buf_.size() );
  CHECK( msg == "hello from the server!" );

  CHECK( !tls_client.is_closed() );
  CHECK( !tls_client.is_closed_for_writing() );
  CHECK( !tls_client.is_closed_for_reading() );
  CHECK( tls_client.is_active() );

  tls_client.close();
  CHECK( !send_buf.empty() );

  mnbytes = co_await client.async_send( send_buf );
  CHECK( mnbytes.value() == send_buf.size() );
  send_buf.clear();

  {
    auto mbuf = co_await client.async_recv();
    CHECK( !mbuf.value().to_bytes().empty() );
    tls_client.received_data( mbuf.value().to_bytes() );
    CHECK( send_buf.empty() );
    CHECK( pcallbacks->close_notify_received_ );
  }

  CHECK( tls_client.is_closed() );
  CHECK( tls_client.is_closed_for_writing() );
  CHECK( tls_client.is_closed_for_reading() );
  CHECK( !tls_client.is_active() );

  ++num_runs;
  co_return;
}

fiona::task<void>
tls_server( fiona::tcp::acceptor acceptor )
{
  fiona::tls::tls_context ctx;
  ctx.add_certificate_key_pair( "../../test/tls/botan/server.crt.pem",
                                "../../test/tls/botan/server.key.pem" );

  auto m_fd = co_await acceptor.async_accept_raw();
  auto ex = acceptor.get_executor();

  CHECK( m_fd.has_value() );
  auto fd = m_fd.value();

  fiona::tls::server stream( ctx, ex, fd );
  ex.register_buffer_sequence( 4 * 1024, 8, 0 );
  stream.set_buffer_group( 0 );
  auto m_ok = co_await stream.async_handshake();
  CHECK( m_ok.has_value() );

  auto m_bufs = co_await stream.async_recv();
  CHECK( m_bufs.has_value() );

  auto msg = m_bufs->to_string();
  CHECK( msg == "hello, world! encryption is great!" );

  auto m_sent = co_await stream.async_send( "hello from the server!" );
  CHECK( m_sent.has_value() );

  m_ok = co_await stream.async_shutdown();
  CHECK( m_ok.has_value() );

  m_ok = co_await stream.async_close();
  CHECK( m_ok.has_value() );

  ++num_runs;
}

fiona::task<void>
tls_client( fiona::executor ex, std::uint16_t const port )
{
  fiona::tls::tls_context ctx;
  ctx.add_certificate_authority( "../../test/tls/botan/ca.crt.pem" );

  fiona::tls::client client( ctx, ex );

  ex.register_buffer_sequence( 4 * 1024, 8, 1 );
  client.set_buffer_group( 1 );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
  auto m_ok = co_await client.async_connect( &addr );
  REQUIRE( m_ok.has_value() );

  m_ok = co_await client.async_handshake();
  CHECK( m_ok.has_value() );

  co_await client.async_send( "hello, world! encryption is great!" );
  auto mbufs = co_await client.async_recv();
  CHECK( mbufs.has_value() );

  auto msg = mbufs.value().to_string();
  CHECK( msg == "hello from the server!" );

  m_ok = co_await client.async_shutdown();
  CHECK( m_ok.has_value() );

  m_ok = co_await client.async_close();
  CHECK( m_ok.has_value() );

  ++num_runs;

  co_return;
}

} // namespace

TEST_CASE( "botan hello world" )
{
  num_runs = 0;

  fiona::io_context ioc;

  auto server_addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ioc.get_executor(), &server_addr );

  auto port = acceptor.port();

  ioc.spawn( server( std::move( acceptor ) ) );
  ioc.spawn( client( ioc.get_executor(), port ) );
  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "tls::client hello world" )
{
  num_runs = 0;

  fiona::io_context ioc;

  auto server_addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ioc.get_executor(), &server_addr );

  auto port = acceptor.port();

  ioc.spawn( tls_server( std::move( acceptor ) ) );
  ioc.spawn( tls_client( ioc.get_executor(), port ) );
  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "large messages" )
{
  struct server_op
  {
    static fiona::task<void>
    recv_op( fiona::tls::server stream, std::span<std::uint8_t const> msg )
    {
      std::vector<std::uint8_t> received_msg;

      while ( received_msg.size() < msg.size() ) {
        auto m_bufs = co_await stream.async_recv();
        CHECK( m_bufs.has_value() );

        auto bufs = std::move( m_bufs.value() );
        for ( auto buf : bufs ) {
          auto m = buf.readable_bytes();
          received_msg.insert( received_msg.end(), m.begin(), m.end() );
        }
      }

      CHECK( received_msg.size() == msg.size() );
      CHECK( std::equal( received_msg.begin(), received_msg.end(), msg.begin(),
                         msg.end() ) );

      co_return;
    }

    static fiona::task<void>
    send_op( fiona::tls::server stream, std::span<std::uint8_t const> msg )
    {
      auto m_sent = co_await stream.async_send( msg );
      CHECK( m_sent.has_value() );
      CHECK( m_sent.value() == msg.size() );
      co_return;
    }

    static fiona::task<void>
    run( fiona::tcp::acceptor acceptor, std::span<std::uint8_t const> msg )
    {
      fiona::tls::tls_context ctx;
      ctx.add_certificate_key_pair( "../../test/tls/botan/server.crt.pem",
                                    "../../test/tls/botan/server.key.pem" );

      auto m_fd = co_await acceptor.async_accept_raw();
      auto ex = acceptor.get_executor();

      CHECK( m_fd.has_value() );
      auto fd = m_fd.value();

      fiona::tls::server stream( ctx, ex, fd );
      ex.register_buffer_sequence( 4 * 1024, 8, 0 );
      stream.set_buffer_group( 0 );
      auto m_ok = co_await stream.async_handshake();
      CHECK( m_ok.has_value() );

      auto h1 = fiona::spawn( ex, recv_op( stream, msg ) );
      auto h2 = fiona::spawn( ex, send_op( stream, msg ) );

      co_await h1;
      co_await h2;

      m_ok = co_await stream.async_shutdown();
      CHECK( m_ok.has_value() );

      m_ok = co_await stream.async_close();
      CHECK( m_ok.has_value() );

      ++num_runs;
      co_return;
    }
  };

  struct client_op
  {
    static fiona::task<void>
    recv_op( fiona::tls::client stream, std::span<std::uint8_t const> msg )
    {
      std::vector<std::uint8_t> received_msg;

      while ( received_msg.size() < msg.size() ) {
        auto m_bufs = co_await stream.async_recv();
        CHECK( m_bufs.has_value() );

        auto bufs = std::move( m_bufs ).value();
        for ( auto buf : bufs ) {
          auto m = buf.readable_bytes();
          received_msg.insert( received_msg.end(), m.begin(), m.end() );
        }
      }

      CHECK( received_msg.size() == msg.size() );
      CHECK( std::equal( received_msg.begin(), received_msg.end(), msg.begin(),
                         msg.end() ) );

      co_return;
    }

    static fiona::task<void>
    send_op( fiona::tls::client stream, std::span<std::uint8_t const> msg )
    {
      auto m_sent = co_await stream.async_send( msg );
      CHECK( m_sent.has_value() );
      CHECK( m_sent.value() == msg.size() );
      co_return;
    }

    static fiona::task<void>
    run( fiona::executor ex,
         std::uint16_t port,
         std::span<std::uint8_t const> msg )
    {
      fiona::tls::tls_context ctx;
      ctx.add_certificate_authority( "../../test/tls/botan/ca.crt.pem" );

      fiona::tls::client client( ctx, ex );

      ex.register_buffer_sequence( 4 * 1024, 8, 1 );
      client.set_buffer_group( 1 );

      auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
      auto m_ok = co_await client.async_connect( &addr );
      REQUIRE( m_ok.has_value() );

      m_ok = co_await client.async_handshake();
      CHECK( m_ok.has_value() );

      auto h1 = fiona::spawn( ex, recv_op( client, msg ) );
      auto h2 = fiona::spawn( ex, send_op( client, msg ) );

      co_await h1;
      co_await h2;

      m_ok = co_await client.async_shutdown();
      CHECK( m_ok.has_value() );

      m_ok = co_await client.async_close();
      CHECK( m_ok.has_value() );

      ++num_runs;

      co_return;
    }
  };

  num_runs = 0;

  std::size_t const msg_size = 2 * 1024 * 1024;
  std::vector<std::uint8_t> msg( msg_size, 0 );
  auto rng = Catch::Generators::random( 0, 255 );
  for ( auto& b : msg ) {
    b = static_cast<std::uint8_t>( rng.get() );
  }

  fiona::io_context ioc;

  auto const addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ioc.get_executor(), &addr );

  auto const port = acceptor.port();

  ioc.spawn( server_op::run( std::move( acceptor ), msg ) );
  ioc.spawn( client_op::run( ioc.get_executor(), port, msg ) );
  ioc.run();

  CHECK( num_runs == 2 );
}

TEST_CASE( "multiple clients" )
{
  static constexpr int const num_clients = 10;

  struct server_op
  {
    static fiona::task<void>
    recv_op( fiona::tls::server stream, std::span<std::uint8_t const> msg )
    {
      std::vector<std::uint8_t> received_msg;

      while ( received_msg.size() < msg.size() ) {
        auto m_bufs = co_await stream.async_recv();
        CHECK( m_bufs.has_value() );

        auto bufs = std::move( m_bufs.value() );
        for ( auto buf : bufs ) {
          auto m = buf.readable_bytes();
          received_msg.insert( received_msg.end(), m.begin(), m.end() );
        }
      }

      CHECK( received_msg.size() == msg.size() );
      CHECK( std::equal( received_msg.begin(), received_msg.end(), msg.begin(),
                         msg.end() ) );

      co_return;
    }

    static fiona::task<void>
    send_op( fiona::tls::server stream, std::span<std::uint8_t const> msg )
    {
      auto m_sent = co_await stream.async_send( msg );
      CHECK( m_sent.has_value() );
      CHECK( m_sent.value() == msg.size() );
      co_return;
    }

    static fiona::task<void>
    session( fiona::tls::server stream, std::span<std::uint8_t const> msg )
    {
      auto ex = stream.get_executor();

      stream.set_buffer_group( 0 );
      auto m_ok = co_await stream.async_handshake();
      CHECK( m_ok.has_value() );

      auto h1 = fiona::spawn( ex, recv_op( stream, msg ) );
      auto h2 = fiona::spawn( ex, send_op( stream, msg ) );

      co_await h1;
      co_await h2;

      m_ok = co_await stream.async_shutdown();
      CHECK( m_ok.has_value() );

      m_ok = co_await stream.async_close();
      CHECK( m_ok.has_value() );

      ++num_runs;
      co_return;
    }

    static fiona::task<void>
    run( fiona::tcp::acceptor acceptor, std::span<std::uint8_t const> msg )
    {
      fiona::tls::tls_context ctx;
      ctx.add_certificate_key_pair( "../../test/tls/botan/server.crt.pem",
                                    "../../test/tls/botan/server.key.pem" );

      auto ex = acceptor.get_executor();
      ex.register_buffer_sequence( 4 * 1024, 1024, 0 );

      for ( int i = 0; i < num_clients; ++i ) {
        auto m_fd = co_await acceptor.async_accept_raw();

        CHECK( m_fd.has_value() );
        auto fd = m_fd.value();

        fiona::tls::server stream( ctx, ex, fd );
        ex.spawn( session( stream, msg ) );
      }

      co_return;
    }
  };

  struct client_op
  {

    static fiona::task<void>
    recv_op( fiona::tls::client stream, std::span<std::uint8_t const> msg )
    {
      std::vector<std::uint8_t> received_msg;

      while ( received_msg.size() < msg.size() ) {
        auto m_bufs = co_await stream.async_recv();
        CHECK( m_bufs.has_value() );

        auto bufs = std::move( m_bufs ).value();
        for ( auto buf : bufs ) {
          auto m = buf.readable_bytes();
          received_msg.insert( received_msg.end(), m.begin(), m.end() );
        }
      }

      CHECK( received_msg.size() == msg.size() );
      CHECK( std::equal( received_msg.begin(), received_msg.end(), msg.begin(),
                         msg.end() ) );

      co_return;
    }

    static fiona::task<void>
    send_op( fiona::tls::client stream, std::span<std::uint8_t const> msg )
    {
      auto m_sent = co_await stream.async_send( msg );
      CHECK( m_sent.has_value() );
      CHECK( m_sent.value() == msg.size() );
      co_return;
    }

    static fiona::task<void>
    session( fiona::tls::tls_context ctx,
             fiona::executor ex,
             std::uint16_t port,
             std::span<std::uint8_t const> msg )
    {
      fiona::tls::client client( ctx, ex );
      client.set_buffer_group( 1 );

      auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
      auto m_ok = co_await client.async_connect( &addr );
      REQUIRE( m_ok.has_value() );

      m_ok = co_await client.async_handshake();
      CHECK( m_ok.has_value() );

      auto h1 = fiona::spawn( ex, recv_op( client, msg ) );
      auto h2 = fiona::spawn( ex, send_op( client, msg ) );

      co_await h1;
      co_await h2;

      m_ok = co_await client.async_shutdown();
      CHECK( m_ok.has_value() );

      m_ok = co_await client.async_close();
      CHECK( m_ok.has_value() );

      ++num_runs;
      co_return;
    }

    static fiona::task<void>
    run( fiona::executor ex,
         std::uint16_t port,
         std::span<std::uint8_t const> msg )
    {
      fiona::tls::tls_context ctx;
      ctx.add_certificate_authority( "../../test/tls/botan/ca.crt.pem" );

      ex.register_buffer_sequence( 4 * 1024, 1024, 1 );

      co_await session( ctx, ex, port, msg );

      for ( int i = 1; i < num_clients; ++i ) {
        ex.spawn( session( ctx, ex, port, msg ) );
      }

      co_return;
    }
  };

  num_runs = 0;

  std::size_t const msg_size = 2 * 1024;
  auto msg = make_random_input( msg_size );

  fiona::io_context ioc;

  auto const addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ioc.get_executor(), &addr );

  auto const port = acceptor.port();

  ioc.spawn( server_op::run( std::move( acceptor ), msg ) );
  ioc.spawn( client_op::run( ioc.get_executor(), port, msg ) );
  ioc.run();

  CHECK( num_runs == 2 * num_clients );
}
