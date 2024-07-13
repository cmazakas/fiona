// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <catch2/catch_test_macros.hpp>

void asio_timer_bench();

void fiona_timer_bench();

TEST_CASE( "asio timer bench" ) { asio_timer_bench(); }
TEST_CASE( "fiona timer bench" ) { fiona_timer_bench(); }
