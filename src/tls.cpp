// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <fiona/tls.hpp>

#include <fiona/buffer.hpp>
#include <fiona/dns.hpp>
#include <fiona/error.hpp>
#include <fiona/executor.hpp>

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

#include "buffers_adaptor.hpp"
#include "stream_impl.hpp"

#include <cerrno>
#include <cstring>
#include <ios>
#include <memory>
#include <vector>

using namespace std::chrono_literals;

namespace fiona {
namespace tls {
namespace detail {

struct tls_callbacks final : public Botan::TLS::Callbacks {
  std::vector<unsigned char> send_buf_;
  recv_buffer_sequence recv_seq_;
  bool received_record_ = false;

  bool close_notify_received_ = false;
  bool failed_cert_verification_ = false;

  ~tls_callbacks() override;

  void tls_emit_data( std::span<std::uint8_t const> data ) override {
    send_buf_.insert( send_buf_.end(), data.begin(), data.end() );
  }

  void tls_record_received( std::uint64_t /* seq_no */,
                            std::span<std::uint8_t const> plaintext ) override {
    received_record_ = true;

    auto pos = recv_seq_.begin();
    auto end = recv_seq_.end();
    while ( !plaintext.empty() ) {
      BOOST_ASSERT( pos != end );
      auto buf_view = *pos;
      auto n = std::min( buf_view.capacity(), plaintext.size() );
      std::memcpy( buf_view.data(), plaintext.data(), n );
      plaintext = plaintext.subspan( n );
      buf_view.set_len( n );
      ++pos;
    }

    for ( ; pos != end; ++pos ) {
      ( *pos ).set_len( 0 );
    }
  }

  void tls_alert( Botan::TLS::Alert alert ) override {
    // if the alert type is a close_notify, we should start a graceful shutdown
    // of the connection, otherwise we're permitted to probably just do a
    // hard shutdown of the TCP connection
    if ( alert.type() == Botan::TLS::AlertType::CloseNotify ) {
      close_notify_received_ = true;
    }
  }

  void tls_verify_cert_chain(
      const std::vector<Botan::X509_Certificate>& cert_chain,
      const std::vector<std::optional<Botan::OCSP::Response>>& ocsp_responses,
      const std::vector<Botan::Certificate_Store*>& trusted_roots,
      Botan::Usage_Type usage, std::string_view hostname,
      const Botan::TLS::Policy& policy ) override {

    Botan::Path_Validation_Restrictions restrictions(
        false, policy.minimum_signature_strength() );

    Botan::Path_Validation_Result result = Botan::x509_path_validate(
        cert_chain, restrictions, trusted_roots, hostname, usage,
        tls_current_timestamp(), 0ms, ocsp_responses );

    if ( !result.successful_validation() ) {
      failed_cert_verification_ = true;
    }
  }
};

tls_callbacks::~tls_callbacks() {}

struct tls_credentials_manager final : public Botan::Credentials_Manager {
  struct cert_key_pair {
    Botan::X509_Certificate cert;
    std::shared_ptr<Botan::Private_Key> key;
  };

  std::vector<cert_key_pair> cert_key_pairs_;
  Botan::Certificate_Store_In_Memory cert_store_;

  tls_credentials_manager() {
    Botan::X509_Certificate cert( "../../test/tls/botan/ca.crt.pem" );
    cert_store_.add_certificate( cert );
  }

  ~tls_credentials_manager() override;

  std::shared_ptr<Botan::Private_Key>
  private_key_for( const Botan::X509_Certificate& cert,
                   const std::string& /* type */,
                   const std::string& /* context */ ) override {
    for ( auto const& [mcert, pkey] : cert_key_pairs_ ) {
      if ( cert == mcert ) {
        return pkey;
      }
    }

    return nullptr;
  }

  std::vector<Botan::X509_Certificate> find_cert_chain(
      const std::vector<std::string>& /* algos */,
      const std::vector<
          Botan::AlgorithmIdentifier>& /* cert_signature_schemes */,
      const std::vector<Botan::X509_DN>& acceptable_cas,
      const std::string& type, const std::string& /* hostname */ ) override {
    if ( type == "tls-server" ) {
      throw "corruption!!!!";
    }

    if ( type == "tls-client" ) {
      for ( const auto& dn : acceptable_cas ) {
        for ( const auto& cred : cert_key_pairs_ ) {
          if ( dn == cred.cert.issuer_dn() ) {
            return { cred.cert };
          }
        }
      }
    }

    return {};
  }

  std::vector<Botan::Certificate_Store*>
  trusted_certificate_authorities( const std::string& type,
                                   const std::string& context ) override {
    BOTAN_UNUSED( type, context );
    // return a list of certificates of CAs we trust for tls server certificates
    // ownership of the pointers remains with Credentials_Manager
    return { &cert_store_ };
  }
};

tls_credentials_manager::~tls_credentials_manager() {}

struct client_impl : public tcp::detail::client_impl {
  std::shared_ptr<tls_callbacks> pcallbacks_;
  std::shared_ptr<Botan::System_RNG> prng_;
  std::shared_ptr<Botan::TLS::Session_Manager_In_Memory> psession_mgr_;
  std::shared_ptr<tls_credentials_manager> pcreds_mgr_;
  std::shared_ptr<Botan::TLS::Policy const> ptls_policy_;
  Botan::TLS::Server_Information server_info_;

  Botan::TLS::Client tls_client_;

  client_impl() = delete;
  client_impl( client_impl const& ) = delete;
  client_impl( client_impl&& ) = delete;

  client_impl( executor ex )
      : tcp::detail::client_impl( ex ),
        pcallbacks_( std::make_shared<tls_callbacks>() ),
        prng_( std::make_shared<Botan::System_RNG>() ),
        psession_mgr_(
            std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng_ ) ),
        pcreds_mgr_( std::make_shared<tls_credentials_manager>() ),
        ptls_policy_( std::make_shared<Botan::TLS::Policy const>() ),
        server_info_( "localhost", 0 ),
        tls_client_( pcallbacks_, psession_mgr_, pcreds_mgr_, ptls_policy_,
                     prng_, server_info_ ) {}

  virtual ~client_impl() override;
};

client_impl::~client_impl() {}

struct server_impl : public tcp::detail::stream_impl {
  std::shared_ptr<tls_callbacks> pcallbacks_;
  std::shared_ptr<Botan::System_RNG> prng_;
  std::shared_ptr<Botan::TLS::Session_Manager_In_Memory> psession_mgr_;
  std::shared_ptr<tls_credentials_manager> pcreds_mgr_;
  std::shared_ptr<Botan::TLS::Policy const> ptls_policy_;

  Botan::TLS::Server tls_server_;

  server_impl() = delete;
  server_impl( server_impl const& ) = delete;
  server_impl( server_impl&& ) = delete;

  server_impl( executor ex )
      : tcp::detail::stream_impl( ex ),
        pcallbacks_( std::make_shared<tls_callbacks>() ),
        prng_( std::make_shared<Botan::System_RNG>() ),
        psession_mgr_(
            std::make_shared<Botan::TLS::Session_Manager_In_Memory>( prng_ ) ),
        pcreds_mgr_( std::make_shared<tls_credentials_manager>() ),
        ptls_policy_( std::make_shared<Botan::TLS::Policy const>() ),
        tls_server_( pcallbacks_, psession_mgr_, pcreds_mgr_, ptls_policy_,
                     prng_ ) {}

  virtual ~server_impl() override;
};

server_impl::~server_impl() = default;

} // namespace detail

client::client( executor ex ) { pstream_ = new detail::client_impl( ex ); }
client::~client() {}

task<result<void>>
client::async_handshake() {
  auto& f = *static_cast<detail::client_impl*>( pstream_.get() );
  auto& tls_client = f.tls_client_;
  auto& send_buf = f.pcallbacks_->send_buf_;

  if ( tls_client.is_active() || tls_client.is_closed() ) {
    fiona::detail::throw_errno_as_error_code( EINVAL );
  }

  if ( send_buf.empty() ) {
    fiona::detail::throw_errno_as_error_code( EINVAL );
  }

  co_await tcp::stream::async_send( send_buf );
  send_buf.clear();

  while ( !tls_client.is_handshake_complete() ) {
    auto mbuffers = co_await tcp::stream::async_recv();
    if ( mbuffers.has_error() ) {
      co_return mbuffers.error();
    }

    auto& buf = mbuffers.value();
    auto data = buf.to_bytes();
    tls_client.received_data( data );

    if ( !send_buf.empty() ) {
      co_await tcp::stream::async_send( send_buf );
      send_buf.clear();
    }
  }

  co_return result<void>{};
}

task<result<std::size_t>>
client::async_send( std::span<unsigned char const> data ) {
  auto& f = *static_cast<detail::client_impl*>( pstream_.get() );
  auto& tls_client = f.tls_client_;
  auto& send_buf = f.pcallbacks_->send_buf_;

  tls_client.send( data );

  auto mn = co_await tcp::stream::async_send( send_buf );
  send_buf.clear();
  if ( mn.has_error() ) {
    co_return mn.error();
  }

  co_return data.size();
}

task<result<std::size_t>>
client::async_send( std::string_view msg ) {
  return async_send(
      { reinterpret_cast<unsigned char const*>( msg.data() ), msg.size() } );
}

task<result<recv_buffer_sequence>>
client::async_recv() {
  auto& f = static_cast<detail::client_impl&>( *pstream_ );
  auto& tls_client = f.tls_client_;
  auto& recv_seq = f.pcallbacks_->recv_seq_;

  while ( !f.pcallbacks_->received_record_ ) {
    auto mbuffers = co_await tcp::stream::async_recv();
    if ( mbuffers.has_error() ) {
      co_return mbuffers.error();
    }

    auto& buffers = mbuffers.value();
    auto data = buffers.to_bytes();
    f.pcallbacks_->recv_seq_.concat( std::move( buffers ) );

    tls_client.received_data( data );
  }

  co_return std::move( recv_seq );
}

task<result<void>>
client::async_shutdown() {
  auto& f = static_cast<detail::client_impl&>( *pstream_ );
  auto& tls_client = f.tls_client_;
  auto& send_buf = f.pcallbacks_->send_buf_;

  tls_client.close();

  auto mwritten = co_await tcp::stream::async_send( send_buf );
  if ( mwritten.has_error() ) {
    co_return mwritten.error();
  }
  send_buf.clear();

  auto mbufs = co_await tcp::stream::async_recv();
  if ( mbufs.has_error() ) {
    co_return mbufs.error();
  }

  auto& bufs = mbufs.value();
  for ( auto buf_view : bufs ) {
    tls_client.received_data( buf_view.readable_bytes() );
  }

  co_return result<void>();
}

task<result<void>>
server::async_handshake() {
  co_return result<void>();
}

} // namespace tls
} // namespace fiona
