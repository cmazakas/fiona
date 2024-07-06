#include "common.hpp"

TEST_CASE( "asio recv bench" ) { asio_recv_bench(); }
TEST_CASE( "fiona recv bench" ) { fiona_recv_bench(); }
