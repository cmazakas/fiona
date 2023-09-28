cmake_minimum_required(VERSION 3.22)
project(fiona LANGUAGES CXX)

if (CMAKE_CXX_VERSION LESS "20")
  message(FATAL_ERROR "fiona is a C++20 library")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(liburing 2.5 REQUIRED)
find_package(Boost 1.82 REQUIRED)

add_library(fiona src/io_context.cpp src/tcp.cpp)
target_include_directories(fiona PUBLIC include)
target_link_libraries(
  fiona
  PUBLIC
  liburing::liburing
  Boost::headers
)

include(CTest)
include(FionaTest)
add_subdirectory(test)
