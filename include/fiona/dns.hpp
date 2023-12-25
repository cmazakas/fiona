#ifndef FIONA_DNS_HPP
#define FIONA_DNS_HPP

#include <fiona/executor.hpp>

#include <boost/core/exchange.hpp>

#include <coroutine>
#include <memory>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace fiona {

struct dns_frame;

struct dns_entry_list {
private:
  addrinfo* head_ = nullptr;

  dns_entry_list( addrinfo* head );

  friend struct dns_awaitable;

public:
  dns_entry_list() = default;

  dns_entry_list( dns_entry_list&& rhs ) noexcept;
  dns_entry_list& operator=( dns_entry_list&& rhs ) noexcept;

  dns_entry_list( dns_entry_list const& ) = delete;
  dns_entry_list& operator=( dns_entry_list const& ) = delete;

  ~dns_entry_list();

  addrinfo const* data() const noexcept;
};

struct dns_awaitable {
private:
  std::shared_ptr<dns_frame> pframe_;

  dns_awaitable( std::shared_ptr<dns_frame> pframe );

  friend struct dns_resolver;

public:
  ~dns_awaitable() = default;

  bool await_ready() const;
  void await_suspend( std::coroutine_handle<> h );
  result<dns_entry_list> await_resume();
};

struct dns_resolver {
private:
  fiona::executor ex_;
  std::shared_ptr<dns_frame> pframe_ = nullptr;

public:
  dns_resolver( fiona::executor ex );

  dns_resolver( dns_resolver&& ) = default;
  dns_resolver& operator=( dns_resolver&& ) = default;

  dns_resolver( dns_resolver const& ) = default;
  dns_resolver& operator=( dns_resolver const& ) = default;

  ~dns_resolver() = default;

  dns_awaitable async_resolve( char const* node, char const* service );
};
} // namespace fiona

#endif // FIONA_DNS_HPP
