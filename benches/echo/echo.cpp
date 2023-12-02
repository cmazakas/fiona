#include <catch2/catch_test_macros.hpp>

void
asio_echo_bench();

void
fiona_echo_bench();

TEST_CASE( "asio echo bench" ) { asio_echo_bench(); }
TEST_CASE( "fiona echo bench" ) { fiona_echo_bench(); }
