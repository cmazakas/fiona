#include <fiona/dns.hpp>

#include <fiona/error.hpp>         // for error_code, result
#include <fiona/executor.hpp>      // for executor, waker

#include <boost/core/exchange.hpp> // for exchange

#include <mutex>                   // for lock_guard, mutex
#include <thread>                  // for jthread

#include <netdb.h> // for addrinfo (ptr only), freeaddrinfo, getaddrinfo

namespace fiona {

struct dns_frame
{
  std::mutex m_;
  std::thread t_;
  char const* node_ = nullptr;
  char const* service_ = nullptr;
  addrinfo const* hints_ = nullptr;
  addrinfo* addrlist_ = nullptr;
  int res_ = -1;
};

dns_entry_list::dns_entry_list( addrinfo* head ) : head_{ head } {}

dns_entry_list::dns_entry_list( dns_entry_list&& rhs ) noexcept
    : head_{ boost::exchange( rhs.head_, nullptr ) }
{
}

dns_entry_list&
dns_entry_list::operator=( dns_entry_list&& rhs ) noexcept
{
  if ( this != &rhs ) {
    head_ = boost::exchange( rhs.head_, nullptr );
  }
  return *this;
}

dns_entry_list::~dns_entry_list()
{
  if ( head_ ) {
    freeaddrinfo( head_ );
  }
}

addrinfo const*
dns_entry_list::data() const noexcept
{
  return head_;
}

dns_awaitable::dns_awaitable( executor ex, std::shared_ptr<dns_frame> pframe )
    : ex_{ ex }, pframe_{ pframe }
{
}

bool
dns_awaitable::await_ready() const
{
  return false;
}

void
dns_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto waker = ex_.make_waker( h );
  auto& frame = *pframe_;
  frame.t_ = std::thread( [pframe = pframe_, waker] {
    int res = -1;

    addrinfo* addrlist = nullptr;
    res = getaddrinfo( pframe->node_, pframe->service_, pframe->hints_,
                       &addrlist );

    {
      std::lock_guard<std::mutex> guard{ pframe->m_ };
      pframe->res_ = res;
      pframe->addrlist_ = addrlist;
    }

    waker.wake();
  } );
}

result<dns_entry_list>
dns_awaitable::await_resume()
{
  auto& frame = *pframe_;
  frame.t_.join();
  std::lock_guard<std::mutex> guard( frame.m_ );

  if ( frame.res_ != 0 ) {
    return { fiona::error_code::from_errno( frame.res_ ) };
  }

  return { dns_entry_list( frame.addrlist_ ) };
}

dns_resolver::dns_resolver( fiona::executor ex )
    : ex_{ ex }, pframe_{ new dns_frame() }
{
}

dns_awaitable
dns_resolver::async_resolve( char const* node, char const* service )
{
  pframe_->node_ = node;
  pframe_->service_ = service;
  return { ex_, pframe_ };
}

} // namespace fiona
