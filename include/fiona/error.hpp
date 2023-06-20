#ifndef FIONA_ERROR_HPP
#define FIONA_ERROR_HPP

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
};

} // namespace fiona

#endif // FIONA_ERROR_HPP
