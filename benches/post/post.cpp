// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "common.hpp"

TEST_CASE( "asio post bench" ) { asio_post_bench(); }
TEST_CASE( "fiona post bench" ) { fiona_post_bench(); }
