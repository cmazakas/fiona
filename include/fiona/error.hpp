#ifndef FIONA_ERROR_HPP
#define FIONA_ERROR_HPP

#include <boost/assert.hpp>        // for BOOST_ASSERT
#include <boost/config.hpp>
#include <boost/system/result.hpp> // for result

#include <cstring>                 // for strerror
#include <iostream>                // for operator<<, basic_ostream:...
#include <system_error>            // for make_error_code, operator<<

namespace fiona {

struct error_code : public std::error_code {
  inline friend std::ostream& operator<<( std::ostream& os, error_code const& ec ) {
    os << static_cast<std::error_code const&>( ec ) << ":" << std::strerror( ec.value() );
    return os;
  }

  static error_code from_errno( int e ) {
    BOOST_ASSERT( e != 0 );
    return { std::make_error_code( static_cast<std::errc>( e ) ) };
  }
};

namespace detail {

BOOST_NOINLINE BOOST_NORETURN inline void
throw_errno_as_error_code( int e ) {
  throw std::system_error( error_code::from_errno( e ) );
}

} // namespace detail

template <class T>
using result = boost::system::result<T, error_code>;

} // namespace fiona

#endif // FIONA_ERROR_HPP
