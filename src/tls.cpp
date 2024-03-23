#include <boost/buffers/detail/type_traits.hpp>
#include <boost/buffers/tag_invoke.hpp>
#include <boost/buffers/type_traits.hpp>
#include <fiona/buffer.hpp>
#include <fiona/dns.hpp>
#include <fiona/error.hpp>
#include <fiona/executor.hpp>
#include <fiona/tls.hpp>

#include <boost/buffers/algorithm.hpp>
#include <boost/buffers/buffer.hpp>
#include <boost/buffers/buffer_copy.hpp>
#include <boost/buffers/const_buffer.hpp>
#include <boost/buffers/mutable_buffer.hpp>

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

struct buffers_iterator : public recv_buffer_sequence_view::iterator {
  using value_type = boost::buffers::mutable_buffer;
  using reference = boost::buffers::mutable_buffer;

  auto as_base() noexcept {
    return static_cast<recv_buffer_sequence_view::iterator*>( this );
  }
  auto as_base() const noexcept {
    return static_cast<recv_buffer_sequence_view::iterator const*>( this );
  }

  boost::buffers::mutable_buffer operator*() const noexcept {
    auto buf_view = **as_base();
    return { buf_view.data(), buf_view.size() };
  }

  buffers_iterator& operator++() {
    as_base()->operator++();
    return *this;
  }

  buffers_iterator operator++( int ) {
    auto old = *this;
    as_base()->operator++( 0 );
    return old;
  }

  buffers_iterator& operator--() {
    as_base()->operator--();
    return *this;
  }

  buffers_iterator operator--( int ) {
    auto old = *this;
    as_base()->operator--( 0 );
    return old;
  }
};

struct buffers_adapter {
  using value_type = boost::buffers::mutable_buffer;
  using const_iterator = buffers_iterator;

  recv_buffer_sequence_view buf_seq_view_;

  buffers_iterator begin() const noexcept { return { buf_seq_view_.begin() }; }
  buffers_iterator end() const noexcept { return { buf_seq_view_.end() }; }
};

static_assert( boost::buffers::detail::is_bidirectional_iterator<
               fiona::recv_buffer_sequence_view::iterator>::value );

static_assert( boost::buffers::detail::is_bidirectional_iterator<
               buffers_iterator>::value );

static_assert(
    boost::buffers::is_mutable_buffer_sequence<buffers_adapter>::value );

static_assert(
    boost::buffers::is_const_buffer_sequence<buffers_adapter>::value );

recv_buffer_sequence_view
tag_invoke( boost::buffers::prefix_tag, buffers_adapter const& seq,
            std::size_t n );

recv_buffer_sequence_view
tag_invoke( boost::buffers::prefix_tag, buffers_adapter const& seq,
            std::size_t n ) {

  std::size_t total_len = 0;
  auto seq_view = seq.buf_seq_view_;
  auto end = seq_view.end();

  for ( auto pos = seq_view.begin(); pos != end; ++pos ) {
    auto buf = *pos;
    auto new_len = total_len + buf.size();
    if ( new_len > n ) {
      buf.set_len( buf.size() - ( new_len - n ) );
    }

    if ( total_len == n ) {
      buf.set_len( 0 );
      continue;
    }

    total_len += buf.size();
  }

  return seq_view;
}

struct client_callbacks final : public Botan::TLS::Callbacks {
  std::vector<unsigned char> recv_buf_;
  std::vector<unsigned char> send_buf_;
  recv_buffer_sequence recv_seq_;

  bool close_notify_received_ = false;
  bool failed_cert_verification_ = false;

  ~client_callbacks() override;

  void tls_emit_data( std::span<std::uint8_t const> data ) override {
    send_buf_.insert( send_buf_.end(), data.begin(), data.end() );
  }

  void tls_record_received( std::uint64_t /* seq_no */,
                            std::span<std::uint8_t const> data ) override {
    std::cout << "tls client received application data" << std::endl;

    auto n = boost::buffers::buffer_copy(
        buffers_adapter{ recv_seq_ },
        boost::buffers::buffer( data.data(), data.size() ) );

    std::cout << "yay, Buffers!" << std::endl;
    auto seq = boost::buffers::prefix( buffers_adapter{ recv_seq_ }, n );
    for ( auto const v : seq ) {
      if ( v.empty() ) {
        continue;
      }
      std::cout << v.as_str() << std::endl;
    }

    recv_buf_.insert( recv_buf_.end(), data.begin(), data.end() );
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

client_callbacks::~client_callbacks() {}

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
  std::shared_ptr<client_callbacks> pcallbacks_;
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
        pcallbacks_( std::make_shared<client_callbacks>() ),
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
    auto mbuf = co_await tcp::stream::async_recv();
    if ( mbuf.has_error() ) {
      co_return mbuf.error();
    }

    auto& buf = mbuf.value();
    auto data = buf.readable_bytes();
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

task<result<std::size_t>>
client::async_recv() {
  auto& f = *static_cast<detail::client_impl*>( pstream_.get() );
  auto& tls_client = f.tls_client_;
  auto& recv_buf = f.pcallbacks_->recv_buf_;

  while ( recv_buf.empty() ) {
    std::cout << "doing a looping read..." << std::endl;
    auto mbuf = co_await tcp::stream::async_recv();
    if ( mbuf.has_error() ) {
      co_return mbuf.error();
    }

    auto& buf = mbuf.value();
    auto data = buf.readable_bytes();
    BOOST_ASSERT( buf.capacity() > 0 );

    f.pcallbacks_->recv_seq_.push_back( std::move( buf ) );
    tls_client.received_data( data );
  }

  co_return recv_buf.size();
}

std::span<unsigned char>
client::buffer() const noexcept {
  auto& f = *static_cast<detail::client_impl*>( pstream_.get() );
  auto& recv_buf = f.pcallbacks_->recv_buf_;
  return recv_buf;
}

} // namespace tls
} // namespace fiona
