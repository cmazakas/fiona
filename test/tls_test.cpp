#include "helpers.hpp"

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

  void tls_record_received(
      uint64_t seq_no, std::span<const uint8_t> data ) override {
    std::cout
        << "this should be where we pass along application data (decrypted)"
        << std::endl;

    (void)seq_no;
    (void)data;
  }

  void tls_alert( Botan::TLS::Alert alert ) override {
    std::cout << "this should be called when the session is terminating"
              << std::endl;
    (void)alert;
  }
};

struct tls_credentials_manager : public Botan::Credentials_Manager {
  std::vector<Botan::X509_Certificate> certs_;
  std::vector<std::shared_ptr<Botan::Private_Key>> keys_;

  tls_credentials_manager() {
    std::string key = "../../test/tls/botan/ca.key.pem";
    REQUIRE( std::filesystem::exists( key ) );
    Botan::DataSource_Stream data_source( key );

    auto pkey = Botan::PKCS8::load_key( data_source );
    REQUIRE( pkey != nullptr );

    std::vector<Botan::X509_Certificate> certs;

    std::string cert = "../../test/tls/botan/ca.crt.pem";
    REQUIRE( std::filesystem::exists( cert ) );

    Botan::DataSource_Stream cert_data_source( cert );
    certs.push_back( Botan::X509_Certificate( cert_data_source ) );
    CHECK( cert_data_source.get_bytes_read() > 0 );
    cert_data_source.discard_next( cert_data_source.get_bytes_read() );
    CHECK( cert_data_source.end_of_data() );

    std::cout << "rawr????" << std::endl;
    certs_ = std::move( certs );
    keys_.push_back( std::move( pkey ) );
  }

  std::shared_ptr<Botan::Private_Key> private_key_for(
      const Botan::X509_Certificate& cert, const std::string& /* type */,
      const std::string& /* context */ ) override {
    for ( auto i = 0u; i < certs_.size(); ++i ) {
      auto const& mcert = certs_[i];
      if ( cert == mcert ) {
        return keys_[i];
      }
    }

    return nullptr;
  }

  std::vector<Botan::X509_Certificate> find_cert_chain(
      const std::vector<std::string>& algos,
      const std::vector<Botan::AlgorithmIdentifier>& cert_signature_schemes,
      const std::vector<Botan::X509_DN>& acceptable_cas,
      const std::string& type, const std::string& hostname ) override {
    BOTAN_UNUSED( cert_signature_schemes );
    BOTAN_UNUSED( acceptable_cas );

    if ( type != "tls-server" ) {
      throw "unsupported";
    }

    for ( auto i = 0u; i < certs_.size(); ++i ) {
      auto const& cert = certs_[i];
      auto const& pkey = keys_[i];

      auto pos = std::find( algos.begin(), algos.end(), pkey->algo_name() );
      if ( pos == algos.end() ) {
        continue;
      }

      if ( !hostname.empty() && cert.matches_dns_name( hostname ) ) {
        return certs_;
      }
    }

    return {};

    // if ( type == "tls-client" ) {
    //   for ( const auto& dn : acceptable_cas ) {
    //     for ( const auto& cred : m_certs ) {
    //       if ( dn == cred.certs[0].issuer_dn() ) {
    //         return cred.certs;
    //       }
    //     }
    //   }
    // } else if ( type == "tls-server" ) {
    //   for ( const auto& i : m_certs ) {
    //     if ( std::find( algos.begin(), algos.end(), i.key->algo_name() ) ==
    //          algos.end() ) {
    //       continue;
    //     }

    //     if ( !hostname.empty() && !i.certs[0].matches_dns_name( hostname ) )
    //     {
    //       continue;
    //     }

    //     return i.certs;
    //   }
    // }
  }
};

fiona::task<void>
server( fiona::tcp::acceptor acceptor ) {
  auto prng = std::make_shared<Botan::System_RNG>();
  auto pcallbacks = std::make_shared<tls_callbacks>();
  auto psession_mgr =
      std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng );

  auto pcreds_mgr = std::make_shared<tls_credentials_manager>();
  auto ptls_policy = std::make_shared<Botan::TLS::Policy const>();

  Botan::TLS::Server tls_server(
      pcallbacks, psession_mgr, pcreds_mgr, ptls_policy, prng );

  CHECK( !tls_server.is_active() );
  CHECK( !tls_server.is_closed() );

  auto mstream = co_await acceptor.async_accept();
  auto& stream = mstream.value();

  auto ex = stream.get_executor();
  ex.register_buffer_sequence( 1024, 128, 0 );

  auto rx = stream.get_receiver( 0 );
  while ( true ) {
    auto mbuf = co_await rx.async_recv();
    auto& buf = mbuf.value();

    CHECK( buf.readable_bytes().size() > 0 );

    std::cout << "going to feed data to server tls session" << std::endl;
    try {
      auto n = tls_server.received_data( buf.readable_bytes() );
      std::cout << "we're looking for " << n << " more bytes from the client"
                << std::endl;

      if ( n == 0 ) {
        break;
      }
    } catch ( ... ) {
      std::cout << "so I am throwing here!" << std::endl;
      throw;
    }
  }

  REQUIRE( pcallbacks->send_buf_.size() > 0 );

  std::cout << "server sent back its part of the handshake" << std::endl;
  auto n = co_await stream.async_send( pcallbacks->send_buf_ );
  REQUIRE( n.value() == pcallbacks->send_buf_.size() );
  pcallbacks->send_buf_.clear();

  ++num_runs;
  co_return;
}

fiona::task<void>
client( fiona::executor ex, std::uint16_t const port ) {
  auto prng = std::make_shared<Botan::System_RNG>();
  auto pcallbacks = std::make_shared<tls_callbacks>();
  auto psession_mgr =
      std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng );

  auto pcreds_mgr = std::make_shared<Botan::Credentials_Manager>();
  auto ptls_policy = std::make_shared<Botan::TLS::Policy const>();

  auto server_info = Botan::TLS::Server_Information( "localhost", 0 );

  Botan::TLS::Client tls_client(
      pcallbacks, psession_mgr, pcreds_mgr, ptls_policy, prng, server_info );

  CHECK( !tls_client.is_active() );
  CHECK( !tls_client.is_closed() );

  fiona::tcp::client client( ex );

  auto addr = fiona::ip::make_sockaddr_ipv4( localhost_ipv4, port );
  co_await client.async_connect( &addr );

  auto n = co_await client.async_send( pcallbacks->send_buf_ );
  CHECK( n == pcallbacks->send_buf_.size() );
  pcallbacks->send_buf_.clear();

  ex.register_buffer_sequence( 1024, 128, 1 );
  auto rx = client.get_receiver( 1 );

  while ( true ) {
    auto mbuf = co_await rx.async_recv();
    auto& buf = mbuf.value();

    auto data = buf.readable_bytes();

    std::cout << "going to feed data to client tls session now" << std::endl;
    auto n = tls_client.received_data( data );
    std::cout << "client read back from the server... (" << n << " bytes)"
              << std::endl;
    if ( n == 0 ) {
      break;
    }
  }

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
