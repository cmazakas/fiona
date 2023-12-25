#include <fiona/dns.hpp>

namespace fiona {

struct dns_frame {
  std::mutex m_;
  std::jthread t_;
  fiona::executor ex_;
  char const* const node_ = nullptr;
  char const* const service_ = nullptr;
  addrinfo const* const hints_ = nullptr;
  addrinfo* addrlist_ = nullptr;
  int res_ = -1;

  dns_frame( fiona::executor ex, char const* node, char const* service,
             addrinfo const* hints )
      : ex_{ ex }, node_{ node }, service_{ service }, hints_{ hints } {}
};

dns_entry_list::dns_entry_list( addrinfo* head ) : head_{ head } {}

dns_entry_list::dns_entry_list( dns_entry_list&& rhs ) noexcept
    : head_{ boost::exchange( rhs.head_, nullptr ) } {}

dns_entry_list&
dns_entry_list::operator=( dns_entry_list&& rhs ) noexcept {
  if ( this != &rhs ) {
    head_ = boost::exchange( rhs.head_, nullptr );
  }
  return *this;
}

dns_entry_list::~dns_entry_list() {
  if ( head_ ) {
    freeaddrinfo( head_ );
  }
}

addrinfo const*
dns_entry_list::data() const noexcept {
  return head_;
}

dns_awaitable::dns_awaitable( std::shared_ptr<dns_frame> pframe )
    : pframe_{ pframe } {}

bool
dns_awaitable::await_ready() const {
  return false;
}

void
dns_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& frame = *pframe_;

  auto waker = frame.ex_.make_waker( h );

  frame.t_ = std::jthread( [pframe = pframe_, waker] {
    int res = -1;

    addrinfo* addrlist = nullptr;
    res = getaddrinfo( pframe->node_, pframe->service_, pframe->hints_,
                       &addrlist );

    {
      std::lock_guard guard{ pframe->m_ };
      pframe->res_ = res;
      pframe->addrlist_ = addrlist;
    }

    waker.wake();
  } );
}

result<dns_entry_list>
dns_awaitable::await_resume() {
  auto& frame = *pframe_;
  std::lock_guard guard( frame.m_ );

  if ( frame.res_ != 0 ) {
    return { fiona::error_code::from_errno( frame.res_ ) };
  }

  return { dns_entry_list( frame.addrlist_ ) };
}

dns_resolver::dns_resolver( fiona::executor ex ) : ex_{ ex } {}

dns_awaitable
dns_resolver::async_resolve( char const* node, char const* service ) {
  pframe_ = std::make_shared<dns_frame>( ex_, node, service, nullptr );
  return { pframe_ };
}
} // namespace fiona
