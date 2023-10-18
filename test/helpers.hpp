#ifndef FIONA_TEST_HELPERS_HPP
#define FIONA_TEST_HELPERS_HPP

#include <fiona/error.hpp>

#include <boost/config.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <chrono>

#define CHECK_GE( T, U ) CHECK( T >= U )
#define CHECK_EQ( T, U ) CHECK( T == U )
#define CHECK_LT( T, U ) CHECK( T < U )

template <class Rep, class Period>
struct duration_guard {
  std::chrono::duration<Rep, Period> expected;
  std::chrono::system_clock::time_point prev;

  duration_guard( std::chrono::duration<Rep, Period> expected_ )
      : expected( expected_ ), prev( std::chrono::system_clock::now() ) {}

  ~duration_guard() {
    auto now = std::chrono::system_clock::now();

    auto elapsed =
        std::chrono::duration_cast<std::chrono::duration<Rep, Period>>( now -
                                                                        prev );
    CHECK_GE( elapsed, expected );
    CHECK_LT( elapsed, expected * 1.05 );
  }
};

CATCH_TRANSLATE_EXCEPTION( fiona::error_code const& ex ) {
  return ex.message();
}

#if defined( BOOST_GCC )

#if !defined( __SANITIZE_ADDRESS__ ) && defined( __OPTIMIZE__ )
// don't want to run the symmetric transfer tests under:
// gcc -O0 -fsanitize=address
//
// see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100897
#define RUN_SYMMETRIC_TRANSFER_TESTS
#endif

#else
#define RUN_SYMMETRIC_TRANSFER_TESTS
#endif

#endif // FIONA_TEST_HELPERS_HPP
