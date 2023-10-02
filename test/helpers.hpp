#ifndef FIONA_TEST_HELPERS_HPP
#define FIONA_TEST_HELPERS_HPP

#include <fiona/error.hpp>

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

#endif // FIONA_TEST_HELPERS_HPP
