#include <catch2/catch_test_macros.hpp>

void
asio_timer_bench();

void
fiona_timer_bench();

TEST_CASE( "asio timer bench" ) { asio_timer_bench(); }
TEST_CASE( "fiona timer bench" ) { fiona_timer_bench(); }
