cmake_minimum_required(VERSION 3.25)
project(fiona LANGUAGES CXX)

if (${CMAKE_CXX_STANDARD} LESS "20")
  message(FATAL_ERROR "fiona is a C++20 library; current CMAKE_CXX_STANDARD is: ${CMAKE_CXX_STANDARD}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(GenerateExportHeader)

find_package(liburing 2.5 REQUIRED)
find_package(
  Boost 1.84
  REQUIRED
  COMPONENTS
    assert
    config
    container_hash
    core
    smart_ptr
    system
    unordered
  )
find_package(Botan 3.3.0 CONFIG COMPONENTS tls)

file(
  GLOB_RECURSE fiona_headers
  CONFIGURE_DEPENDS
  "include/*.hpp"
)

file(
  GLOB fiona_sources
  CONFIGURE_DEPENDS
  "src/*.cpp"
)

add_library(fiona ${fiona_sources})

target_include_directories(fiona PUBLIC include)
target_include_directories(fiona PRIVATE src/detail)

target_link_libraries(
  fiona
  PUBLIC
  liburing::liburing
  Boost::assert
  Boost::config
  Boost::container_hash
  Boost::core
  Boost::smart_ptr
  Boost::system
  Boost::unordered
)

target_sources(
  fiona
  PUBLIC FILE_SET HEADERS
  BASE_DIRS include
  FILES ${fiona_headers}
)

generate_export_header(fiona)
target_include_directories(fiona PUBLIC ${CMAKE_BINARY_DIR})

add_library(Fiona::core ALIAS fiona)

if(Botan_FOUND)
  file(
    GLOB fiona_tls_sources
    CONFIGURE_DEPENDS
    "src/tls/*.cpp"
  )

  add_library(fiona_tls ${fiona_tls_sources})
  target_link_libraries(
    fiona_tls
    PUBLIC
    Fiona::core
    Botan::Botan-static
  )
  target_include_directories(fiona_tls PRIVATE src/detail)

  generate_export_header(fiona_tls)
  target_include_directories(fiona_tls PUBLIC ${CMAKE_BINARY_DIR})

  add_library(Fiona::tls ALIAS fiona_tls)
endif()

option(FIONA_BUILD_TESTING "" OFF)
if(FIONA_BUILD_TESTING)
  set_target_properties(fiona PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
  include(CTest)
  add_subdirectory(test)
  # add_subdirectory(benches)
endif()
