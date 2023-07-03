#ifndef FIONA_ERROR_HPP
#define FIONA_ERROR_HPP

#include <boost/config.hpp>

#include <cstring>
#include <iostream>
#include <system_error>

namespace fiona {

struct error_code : public std::error_code {
  inline friend std::ostream& operator<<(std::ostream& os,
                                         error_code const& ec) {
    os << static_cast<std::error_code const&>(ec) << ":"
       << std::strerror(ec.value());
    return os;
  }

  static error_code from_errno(int e) {
    return {std::make_error_code(static_cast<std::errc>(e))};
  }
};

namespace detail {

BOOST_NOINLINE BOOST_NORETURN inline void
throw_errno_as_error_code(int e) {
  throw error_code::from_errno(e);
}

} // namespace detail

} // namespace fiona

#endif // FIONA_ERROR_HPP
