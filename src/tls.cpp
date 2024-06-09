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

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <ios>
#include <memory>
#include <vector>

#include "buffers_adaptor.hpp"
#include "stream_impl.hpp"

using namespace std::chrono_literals;

namespace fiona {
namespace tls {
namespace detail {

struct tls_credentials_manager final : public Botan::Credentials_Manager
{
  struct cert_key_pair
  {
    Botan::X509_Certificate cert;
    std::shared_ptr<Botan::Private_Key> key;
  };

  std::vector<cert_key_pair> cert_key_pairs_;
  // todo: someday, this will likely need to be customized based on user input
  // consider:
  // https://botan.randombit.net/doxygen/classBotan_1_1Certificate__Store.html
  Botan::Certificate_Store_In_Memory cert_store_;

  tls_credentials_manager() = default;
  ~tls_credentials_manager() override;

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
      std::vector<
          Botan::AlgorithmIdentifier> const& /* cert_signature_schemes */,
      std::vector<Botan::X509_DN> const& acceptable_cas,
      std::string const& type,
      std::string const& hostname ) override
  {

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

//-----------------------------------------------------------------------------

struct tls_context_frame
{
  std::shared_ptr<Botan::System_RNG> rng_;
  std::shared_ptr<Botan::TLS::Session_Manager_In_Memory> session_mgr_;
  std::shared_ptr<tls_credentials_manager> creds_mgr_;
  std::shared_ptr<Botan::TLS::Policy> policy_;

  tls_context_frame()
      : rng_( std::make_shared<Botan::System_RNG>() ),
        session_mgr_(
            std::make_shared<Botan::TLS::Session_Manager_In_Memory>( rng_ ) ),
        creds_mgr_( std::make_shared<tls_credentials_manager>() ),
        policy_( std::make_shared<Botan::TLS::Policy>() )
  {
  }
};

//-----------------------------------------------------------------------------

struct tls_callbacks final : public Botan::TLS::Callbacks
{
  std::vector<unsigned char> send_buf_;
  recv_buffer_sequence input_sequence_;
  recv_buffer_sequence output_sequence_;

  bool received_record_ = false;
  bool close_notify_received_ = false;
  bool failed_cert_verification_ = false;

  tls_callbacks() = default;
  tls_callbacks( tls_callbacks const& ) = delete;
  tls_callbacks& operator=( tls_callbacks const& ) = delete;

  ~tls_callbacks() override;

  void
  tls_emit_data( std::span<std::uint8_t const> data ) override
  {
    send_buf_.insert( send_buf_.end(), data.begin(), data.end() );
  }

  void
  tls_record_received( std::uint64_t /* seq_no */,
                       std::span<std::uint8_t const> plaintext ) override
  {
    received_record_ = true;

    while ( !plaintext.empty() ) {
      auto buf = output_sequence_.pop_front();

      recv_buffer_view bv = buf;
      bv.set_len( 0 );
      auto dst = bv.spare_capacity_mut();
      auto n = std::min( dst.size(), plaintext.size() );
      std::memcpy( dst.data(), plaintext.data(), n );
      bv.set_len( n );

      input_sequence_.push_back( std::move( buf ) );

      plaintext = plaintext.subspan( n );
    }
  }

  void
  tls_alert( Botan::TLS::Alert alert ) override
  {
    // if the alert type is a close_notify, we should start a graceful shutdown
    // of the connection, otherwise we're permitted to probably just do a
    // hard shutdown of the TCP connection
    if ( alert.type() == Botan::TLS::AlertType::CloseNotify ) {
      close_notify_received_ = true;
    }
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

    if ( !result.successful_validation() ) {
      failed_cert_verification_ = true;
    }
  }
};

tls_callbacks::~tls_callbacks() = default;

//------------------------------------------------------------------------------

struct client_impl : public tcp::detail::client_impl
{
  std::shared_ptr<tls_callbacks> p_callbacks_;
  tls_context tls_ctx_;
  Botan::TLS::Server_Information server_info_;
  Botan::TLS::Client tls_client_;

  client_impl() = delete;
  client_impl( client_impl const& ) = delete;
  client_impl& operator=( client_impl const& ) = delete;

  client_impl( tls_context ctx, executor ex )
      : tcp::detail::client_impl( ex ), p_callbacks_( new tls_callbacks() ),
        tls_ctx_( ctx ), server_info_( "localhost", 0 ),
        tls_client_( p_callbacks_,
                     tls_ctx_.p_tls_frame_->session_mgr_,
                     tls_ctx_.p_tls_frame_->creds_mgr_,
                     tls_ctx_.p_tls_frame_->policy_,
                     tls_ctx_.p_tls_frame_->rng_,
                     server_info_ )
  {
  }

  virtual ~client_impl() override;
};

client_impl::~client_impl() {}

//------------------------------------------------------------------------------

struct server_impl : public tcp::detail::stream_impl
{
  std::shared_ptr<tls_callbacks> p_callbacks_;
  tls_context tls_ctx_;
  Botan::TLS::Server tls_server_;

  server_impl() = delete;
  server_impl( server_impl const& ) = delete;
  server_impl( server_impl&& ) = delete;

  server_impl( tls_context ctx, executor ex, int fd )
      : tcp::detail::stream_impl( ex, fd ),
        p_callbacks_( std::make_shared<tls_callbacks>() ), tls_ctx_( ctx ),
        tls_server_( p_callbacks_,
                     tls_ctx_.p_tls_frame_->session_mgr_,
                     tls_ctx_.p_tls_frame_->creds_mgr_,
                     tls_ctx_.p_tls_frame_->policy_,
                     tls_ctx_.p_tls_frame_->rng_ )
  {
  }

  virtual ~server_impl() override;
};

server_impl::~server_impl() = default;

} // namespace detail

//------------------------------------------------------------------------------

tls_context::tls_context() : p_tls_frame_( new detail::tls_context_frame() ) {}

void
tls_context::add_certificate_authority( std::string_view filepath )
{
  auto& cert_store = p_tls_frame_->creds_mgr_->cert_store_;

  Botan::X509_Certificate cert( filepath );
  cert_store.add_certificate( cert );
}

void
tls_context::add_certificate_key_pair( std::string_view cert_path,
                                       std::string_view key_path )
{
  auto& cert_key_pairs = p_tls_frame_->creds_mgr_->cert_key_pairs_;

  Botan::DataSource_Stream data_source( key_path );

  auto p_key = Botan::PKCS8::load_key( data_source );

  Botan::X509_Certificate cert( cert_path );

  using cert_key_pair = detail::tls_credentials_manager::cert_key_pair;
  cert_key_pairs.emplace_back(
      cert_key_pair{ std::move( cert ), std::move( p_key ) } );
}

//------------------------------------------------------------------------------

client::client( tls_context ctx, executor ex )
{
  pstream_ = new detail::client_impl( ctx, ex );
}

client::~client() {}

task<result<void>>
client::async_handshake()
{
  auto& f = *static_cast<detail::client_impl*>( pstream_.get() );
  auto& tls_client = f.tls_client_;
  auto& send_buf = f.p_callbacks_->send_buf_;

  if ( tls_client.is_active() || tls_client.is_closed() ) {
    co_return error_code::from_errno( EINVAL );
  }

  if ( send_buf.empty() ) {
    co_return error_code::from_errno( EINVAL );
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
client::async_send( std::span<unsigned char const> data )
{
  auto& f = *static_cast<detail::client_impl*>( pstream_.get() );
  auto& tls_client = f.tls_client_;
  auto& send_buf = f.p_callbacks_->send_buf_;

  tls_client.send( data );

  auto mn = co_await tcp::stream::async_send( send_buf );
  send_buf.clear();
  if ( mn.has_error() ) {
    co_return mn.error();
  }

  co_return data.size();
}

task<result<std::size_t>>
client::async_send( std::string_view msg )
{
  return async_send(
      { reinterpret_cast<unsigned char const*>( msg.data() ), msg.size() } );
}

task<result<recv_buffer_sequence>>
client::async_recv()
{
  auto& f = static_cast<detail::client_impl&>( *pstream_ );
  auto& tls_client = f.tls_client_;

  while ( !f.p_callbacks_->received_record_ ) {
    auto m_buffers = co_await tcp::stream::async_recv();
    if ( m_buffers.has_error() ) {
      co_return m_buffers.error();
    }

    auto bufs = std::move( m_buffers ).value();
    while ( !bufs.empty() ) {
      auto buf = bufs.pop_front();
      auto data = buf.readable_bytes();
      f.p_callbacks_->output_sequence_.push_back( std::move( buf ) );

      tls_client.received_data( data );
    }
  }

  f.p_callbacks_->received_record_ = false;
  co_return std::move( f.p_callbacks_->input_sequence_ );
}

task<result<void>>
client::async_shutdown()
{
  auto& f = static_cast<detail::client_impl&>( *pstream_ );
  auto& tls_client = f.tls_client_;
  auto& send_buf = f.p_callbacks_->send_buf_;

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

//------------------------------------------------------------------------------

server::server( tls_context ctx, executor ex, int fd )
{
  pstream_ = new detail::server_impl( ctx, ex, fd );
}

server::~server() = default;

task<result<void>>
server::async_handshake()
{
  auto& f = *static_cast<detail::server_impl*>( pstream_.get() );
  auto& tls_server = f.tls_server_;
  auto& send_buf = f.p_callbacks_->send_buf_;

  if ( tls_server.is_active() || tls_server.is_closed() ) {
    co_return error_code::from_errno( EINVAL );
  }

  while ( !tls_server.is_handshake_complete() ) {
    auto mbuffers = co_await tcp::stream::async_recv();
    if ( mbuffers.has_error() ) {
      co_return mbuffers.error();
    }

    auto& buf = mbuffers.value();
    auto data = buf.to_bytes();
    tls_server.received_data( data );

    if ( !send_buf.empty() ) {
      co_await tcp::stream::async_send( send_buf );
      send_buf.clear();
    }
  }

  co_return result<void>{};
}

task<result<recv_buffer_sequence>>
server::async_recv()
{
  auto& f = static_cast<detail::server_impl&>( *pstream_ );
  auto& tls_server = f.tls_server_;

  while ( !f.p_callbacks_->received_record_ ) {
    auto m_buffers = co_await tcp::stream::async_recv();
    if ( m_buffers.has_error() ) {
      co_return m_buffers.error();
    }

    auto bufs = std::move( m_buffers ).value();
    while ( !bufs.empty() ) {
      auto buf = bufs.pop_front();
      auto data = buf.readable_bytes();
      f.p_callbacks_->output_sequence_.push_back( std::move( buf ) );

      tls_server.received_data( data );
    }
  }

  f.p_callbacks_->received_record_ = false;
  co_return std::move( f.p_callbacks_->input_sequence_ );
}

task<result<std::size_t>>
server::async_send( std::span<unsigned char const> data )
{
  auto& f = *static_cast<detail::server_impl*>( pstream_.get() );
  auto& tls_server = f.tls_server_;
  auto& send_buf = f.p_callbacks_->send_buf_;

  tls_server.send( data );

  auto mn = co_await tcp::stream::async_send( send_buf );
  send_buf.clear();
  if ( mn.has_error() ) {
    co_return mn.error();
  }

  co_return data.size();
}

task<result<std::size_t>>
server::async_send( std::string_view msg )
{
  return async_send(
      { reinterpret_cast<unsigned char const*>( msg.data() ), msg.size() } );
}

task<result<void>>
server::async_shutdown()
{
  auto& f = static_cast<detail::server_impl&>( *pstream_ );
  auto& tls_server = f.tls_server_;
  auto& send_buf = f.p_callbacks_->send_buf_;

  auto m_bufs = co_await tcp::stream::async_recv();
  tls_server.received_data( m_bufs.value().to_bytes() );
  if ( !f.p_callbacks_->close_notify_received_ ) {
    co_return error_code::from_errno( EINVAL );
  }

  auto mnbytes = co_await tcp::stream::async_send( send_buf );
  if ( mnbytes.has_error() ) {
    co_return error_code::from_errno( EINVAL );
  }
  send_buf.clear();

  co_return result<void>();
}
} // namespace tls
} // namespace fiona
