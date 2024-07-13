// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_BENCHES_TIMER_COMMON_HPP
#define FIONA_BENCHES_TIMER_COMMON_HPP

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

void fiona_timer_bench();

void asio_timer_bench();

#endif // FIONA_BENCHES_TIMER_COMMON_HPP
