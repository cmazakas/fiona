cmake_minimum_required(VERSION 3.25)
project(fiona LANGUAGES CXX VERSION 0.1.0)

if (CMAKE_CXX_STANDARD LESS "20")
  message(FATAL_ERROR "fiona is a C++20 library; current CMAKE_CXX_STANDARD is: ${CMAKE_CXX_STANDARD}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(GenerateExportHeader)
include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)

pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)

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

target_include_directories(
  fiona
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  fiona
  PUBLIC
  PkgConfig::uring
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

target_sources(
  fiona
  PUBLIC
  FILE_SET HEADERS
  FILES "${CMAKE_CURRENT_BINARY_DIR}/fiona_export.h"
  BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
)

add_library(Fiona::core ALIAS fiona)

install(
  TARGETS
    fiona

  EXPORT
    fiona-targets

  FILE_SET HEADERS

  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

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
  target_sources(
    fiona_tls
    PUBLIC
    FILE_SET HEADERS
    FILES "${CMAKE_CURRENT_BINARY_DIR}/fiona_tls_export.h"
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
  )

  add_library(Fiona::tls ALIAS fiona_tls)
  install(
    TARGETS
      fiona_tls

    EXPORT
      fiona-targets

    FILE_SET HEADERS

    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

install(
  EXPORT
    fiona-targets

  NAMESPACE Fiona::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fiona
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/fiona-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/fiona-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/fiona-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fiona
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/fiona-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/fiona-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fiona
)

option(FIONA_BUILD_TESTING "" OFF)
if(FIONA_BUILD_TESTING)
  set_target_properties(fiona PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
  include(CTest)
  add_subdirectory(test)
  add_subdirectory(benches)
endif()
