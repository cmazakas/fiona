#ifndef FIONA_DNS_HPP
#define FIONA_DNS_HPP

#include <fiona/error.hpp>         // for result
#include <fiona/executor.hpp>      // for executor

#include <fiona/detail/config.hpp> // for FIONA_DECL

#include <coroutine>               // for coroutine_handle
#include <memory>                  // for shared_ptr

namespace fiona {
struct dns_frame;
} // namespace fiona
struct addrinfo;

namespace fiona {

struct dns_entry_list
{
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

  FIONA_DECL
  ~dns_entry_list();

  FIONA_DECL
  addrinfo const* data() const noexcept;
};

struct dns_awaitable
{
private:
  executor ex_;
  std::shared_ptr<dns_frame> pframe_;

  dns_awaitable( executor ex, std::shared_ptr<dns_frame> pframe );

  friend struct dns_resolver;

public:
  ~dns_awaitable() = default;

  FIONA_DECL
  bool await_ready() const;

  FIONA_DECL
  void await_suspend( std::coroutine_handle<> h );

  FIONA_DECL
  result<dns_entry_list> await_resume();
};

struct dns_resolver
{
private:
  fiona::executor ex_;
  std::shared_ptr<dns_frame> pframe_ = nullptr;

public:
  FIONA_DECL
  dns_resolver( fiona::executor ex );

  dns_resolver( dns_resolver&& ) = default;
  dns_resolver& operator=( dns_resolver&& ) = default;

  dns_resolver( dns_resolver const& ) = default;
  dns_resolver& operator=( dns_resolver const& ) = default;

  ~dns_resolver() = default;

  FIONA_DECL
  dns_awaitable async_resolve( char const* node, char const* service );
};
} // namespace fiona

#endif // FIONA_DNS_HPP
