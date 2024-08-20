// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_BENCHES_ECHO_COMMON_HPP
#define FIONA_BENCHES_ECHO_COMMON_HPP

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <atomic>

inline constexpr char const* localhost_ipv4 = "127.0.0.1";

constexpr int num_clients = 5000;
constexpr int num_msgs = 1000;

using lock_guard = std::lock_guard<std::mutex>;

inline lock_guard
guard()
{
  static std::mutex mtx;
  return lock_guard( mtx );
}

void fiona_echo_bench();

void asio_echo_bench();

#endif // FIONA_BENCHES_ECHO_COMMON_HPP
