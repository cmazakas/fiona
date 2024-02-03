#include "helpers.hpp"

#include <botan/certstor.h>
#include <botan/pk_keys.h>
#include <botan/pkix_enums.h>
#include <botan/x509cert.h>
#include <botan/x509path.h>

#include <fiona/executor.hpp>
#include <fiona/io_context.hpp>
#include <fiona/ip.hpp>
#include <fiona/tcp.hpp>
#include <fiona/time.hpp>

#include <botan/credentials_manager.h>
#include <botan/data_src.h>
#include <botan/pkcs8.h>
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
#include <botan/x509self.h>

#include <filesystem>
#include <memory>
#include <vector>

static int num_runs = 0;

struct tls_callbacks final : public Botan::TLS::Callbacks {
  std::vector<std::uint8_t> send_buf_;

  void tls_emit_data( std::span<uint8_t const> data ) override {
    std::cout << "this needs to be buffered up in a send buffer" << std::endl;
    send_buf_.insert( send_buf_.end(), data.begin(), data.end() );
  };

  void tls_record_received( uint64_t seq_no, std::span<const uint8_t> data ) override {
    std::cout << "this should be where we pass along application data (decrypted)" << std::endl;

    (void)seq_no;
    (void)data;
  }

  void tls_alert( Botan::TLS::Alert alert ) override {
    std::cout << "this should be called when the session is terminating" << std::endl;
    (void)alert;
  }

  void tls_session_activated() override { std::cout << "handshake completed!!!!" << std::endl; }

  void tls_session_established( const Botan::TLS::Session_Summary& /* session */ ) override {
    std::cout << "session established!" << std::endl;
  }

  void tls_verify_cert_chain( const std::vector<Botan::X509_Certificate>& cert_chain,
                              const std::vector<std::optional<Botan::OCSP::Response>>& ocsp_responses,
                              const std::vector<Botan::Certificate_Store*>& trusted_roots, Botan::Usage_Type usage,
                              std::string_view hostname, const Botan::TLS::Policy& policy ) override {

    Botan::Path_Validation_Restrictions restrictions( false, policy.minimum_signature_strength() );

    Botan::Path_Validation_Result result = Botan::x509_path_validate(
        cert_chain, restrictions, trusted_roots, hostname, usage, tls_current_timestamp(), 0ms, ocsp_responses );

    CHECK( result.successful_validation() );
    std::cout << result.result_string() << std::endl;
  }
};

struct tls_credentials_manager : public Botan::Credentials_Manager {
  struct cert_key_pair {
    Botan::X509_Certificate cert;
    std::shared_ptr<Botan::Private_Key> key;
  };

  std::vector<cert_key_pair> ckps_;
  Botan::Certificate_Store_In_Memory cert_store_;

  tls_credentials_manager() {
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

    auto constraints = cert.constraints();
    std::cout << "Hopefully this helps: " << constraints.to_string() << std::endl;
    // CHECK( constraints.includes_any( Botan::Key_Constraints::NonRepudiation ) );
    // CHECK( constraints.includes_any( Botan::Key_Constraints::DigitalSignature ) );

    CHECK( cert_data_source.get_bytes_read() > 0 );
    cert_data_source.discard_next( cert_data_source.get_bytes_read() );
    CHECK( cert_data_source.end_of_data() );

    ckps_.emplace_back( cert_key_pair{ std::move( cert ), std::move( pkey ) } );
  }

  std::shared_ptr<Botan::Private_Key> private_key_for( const Botan::X509_Certificate& cert,
                                                       const std::string& /* type */,
                                                       const std::string& /* context */ ) override {
    for ( auto const& [mcert, pkey] : ckps_ ) {
      if ( cert == mcert ) {
        return pkey;
      }
    }

    return nullptr;
  }

  std::vector<Botan::X509_Certificate>
  find_cert_chain( const std::vector<std::string>& algos,
                   const std::vector<Botan::AlgorithmIdentifier>& cert_signature_schemes,
                   const std::vector<Botan::X509_DN>& acceptable_cas, const std::string& type,
                   const std::string& hostname ) override {
    BOTAN_UNUSED( cert_signature_schemes );
    BOTAN_UNUSED( acceptable_cas );

    if ( type == "tls-server" ) {
      for ( auto const& [cert, pkey] : ckps_ ) {
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
      for ( const auto& dn : acceptable_cas ) {
        for ( const auto& cred : ckps_ ) {
          if ( dn == cred.cert.issuer_dn() ) {
            return { cred.cert };
          }
        }
      }
    }

    return {};
  }

  std::vector<Botan::Certificate_Store*> trusted_certificate_authorities( const std::string& type,
                                                                          const std::string& context ) override {
    BOTAN_UNUSED( type, context );
    // return a list of certificates of CAs we trust for tls server certificates
    // ownership of the pointers remains with Credentials_Manager
    return { &cert_store_ };
  }
};

fiona::task<void>
server( fiona::tcp::acceptor acceptor ) {
  auto prng = std::make_shared<Botan::System_RNG>();
  auto pcallbacks = std::make_shared<tls_callbacks>();
  auto psession_mgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng );

  auto pcreds_mgr = std::make_shared<tls_credentials_manager>();
  auto ptls_policy = std::make_shared<Botan::TLS::Policy const>();

  Botan::TLS::Server tls_server( pcallbacks, psession_mgr, pcreds_mgr, ptls_policy, prng );

  CHECK( !tls_server.is_active() );
  CHECK( !tls_server.is_closed() );

  auto mstream = co_await acceptor.async_accept();
  auto& stream = mstream.value();

  auto ex = stream.get_executor();
  ex.register_buffer_sequence( 1024, 128, 0 );

  auto rx = stream.get_receiver( 0 );
  while ( !tls_server.is_handshake_complete() ) {
    auto mbuf = co_await rx.async_recv();
    auto& buf = mbuf.value();

    CHECK( buf.readable_bytes().size() > 0 );

    std::cout << "going to feed data to server tls session" << std::endl;
    auto n = tls_server.received_data( buf.readable_bytes() );
    std::cout << "we're looking for " << n << " more bytes from the client" << std::endl;

    if ( !pcallbacks->send_buf_.empty() ) {
      co_await stream.async_send( pcallbacks->send_buf_ );
      pcallbacks->send_buf_.clear();
    }
  }

  CHECK( pcallbacks->send_buf_.empty() );
  CHECK( tls_server.is_active() );
  CHECK( tls_server.is_handshake_complete() );

  ++num_runs;
  co_return;
}

fiona::task<void>
client( fiona::executor ex, std::uint16_t const port ) {
  auto prng = std::make_shared<Botan::System_RNG>();
  auto pcallbacks = std::make_shared<tls_callbacks>();
  auto psession_mgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng );

  auto pcreds_mgr = std::make_shared<tls_credentials_manager>();
  auto ptls_policy = std::make_shared<Botan::TLS::Policy const>();

  auto server_info = Botan::TLS::Server_Information( "localhost", 0 );

  Botan::TLS::Client tls_client( pcallbacks, psession_mgr, pcreds_mgr, ptls_policy, prng, server_info );

  CHECK( !tls_client.is_active() );
  CHECK( !tls_client.is_closed() );

  fiona::tcp::client client( ex );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
  co_await client.async_connect( &addr );

  REQUIRE( !pcallbacks->send_buf_.empty() );

  co_await client.async_send( pcallbacks->send_buf_ );
  pcallbacks->send_buf_.clear();

  ex.register_buffer_sequence( 1024, 128, 1 );
  auto rx = client.get_receiver( 1 );

  while ( !tls_client.is_handshake_complete() ) {
    auto mbuf = co_await rx.async_recv();
    auto& buf = mbuf.value();

    auto data = buf.readable_bytes();

    std::cout << "going to feed data to client tls session now" << std::endl;
    auto n = tls_client.received_data( data );
    std::cout << "client read back from the server... (" << n << " bytes)" << std::endl;

    if ( !pcallbacks->send_buf_.empty() ) {
      co_await client.async_send( pcallbacks->send_buf_ );
      pcallbacks->send_buf_.clear();
    }
  }

  CHECK( pcallbacks->send_buf_.empty() );
  CHECK( tls_client.is_active() );
  CHECK( tls_client.is_handshake_complete() );

  ++num_runs;
  co_return;
}

TEST_CASE( "tls_test - botan hello world" ) {
  num_runs = 0;

  fiona::io_context ioc;

  auto server_addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, 0 );
  fiona::tcp::acceptor acceptor( ioc.get_executor(), &server_addr );

  auto port = acceptor.port();

  ioc.post( server( std::move( acceptor ) ) );
  ioc.post( client( ioc.get_executor(), port ) );
  ioc.run();

  CHECK( num_runs == 2 );
}