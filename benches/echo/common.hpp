#ifndef FIONA_BENCHES_ECHO_COMMON_HPP
#define FIONA_BENCHES_ECHO_COMMON_HPP

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_translate_exception.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <span>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>

using namespace std::chrono_literals;

static in_addr const localhost_ipv4 = { .s_addr = htonl( 0x7f000001 ) };

constexpr int num_clients = 5000;
constexpr int num_msgs = 1000;

#endif // FIONA_BENCHES_ECHO_COMMON_HPP
