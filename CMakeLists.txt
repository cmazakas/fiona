cmake_minimum_required(VERSION 3.25)
project(fiona LANGUAGES CXX)

if (${CMAKE_CXX_STANDARD} LESS "20")
  message(FATAL_ERROR "fiona is a C++20 library; current CMAKE_CXX_STANDARD is: ${CMAKE_CXX_STANDARD}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(liburing 2.5 REQUIRED)
find_package(Boost 1.83 REQUIRED)
find_package(Botan 3.3.0 COMPONENTS tls)

set(fiona_headers

  include/fiona/borrowed_buffer.hpp
  include/fiona/dns.hpp
  include/fiona/error.hpp
  include/fiona/executor.hpp
  include/fiona/io_context.hpp
  include/fiona/ip.hpp
  include/fiona/params.hpp
  include/fiona/task.hpp
  include/fiona/tcp.hpp
  include/fiona/time.hpp
  include/fiona/tls.hpp

  include/fiona/detail/common.hpp
  include/fiona/detail/get_sqe.hpp
  include/fiona/detail/time.hpp)

set(fiona_sources

  src/awaitable_base.hpp
  src/stream_impl.hpp

  src/dns.cpp
  src/common.cpp
  src/io_context.cpp
  src/ip.cpp
  src/time.cpp
  src/tcp.cpp)

add_library(fiona ${fiona_sources})

target_include_directories(fiona PUBLIC include)

target_link_libraries(
  fiona
  PUBLIC
  liburing::liburing
  Boost::headers
)

target_sources(
  fiona
  PUBLIC FILE_SET HEADERS
  BASE_DIRS include
  FILES ${fiona_headers}
)

target_compile_definitions(fiona PRIVATE FIONA_SOURCE)
if (BUILD_SHARED_LIBS)
  target_compile_definitions(fiona PRIVATE FIONA_DYN_LINK)
endif()

add_library(Fiona::fiona ALIAS fiona)

if(Botan_FOUND)
  add_library(fiona_tls src/tls.cpp)
  target_link_libraries(fiona_tls PUBLIC fiona Botan::Botan)
  add_library(Fiona::tls ALIAS fiona_tls)

  target_compile_definitions(fiona_tls PRIVATE FIONA_SOURCE)
  if (BUILD_SHARED_LIBS)
    target_compile_definitions(fiona_tls PRIVATE FIONA_DYN_LINK)
  endif()
endif()

option(FIONA_BUILD_TESTING "" OFF)
if(FIONA_BUILD_TESTING)
  set_target_properties(fiona PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
  include(CTest)
  include(FionaTest)
  add_subdirectory(test)
  add_subdirectory(benches)
endif()
