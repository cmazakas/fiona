#include <catch2/catch_test_macros.hpp>

void
asio_post_bench();

void
fiona_post_bench();

TEST_CASE( "asio post bench" ) { asio_post_bench(); }
TEST_CASE( "fiona post bench" ) { fiona_post_bench(); }
