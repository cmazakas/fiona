#ifndef FIONA_BENCHES_ECHO_COMMON_HPP
#define FIONA_BENCHES_ECHO_COMMON_HPP

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>

inline constexpr char const* localhost_ipv4 = "127.0.0.1";

constexpr int num_clients = 5000;
constexpr int num_msgs = 1000;

void fiona_echo_bench();

void asio_echo_bench();

#endif // FIONA_BENCHES_ECHO_COMMON_HPP
