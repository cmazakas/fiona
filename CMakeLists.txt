cmake_minimum_required(VERSION 3.25)
project(fiona LANGUAGES CXX)

if (CXX_STANDARD LESS "20")
  message(FATAL_ERROR "fiona is a C++20 library")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(liburing 2.5 REQUIRED)
find_package(Boost 1.82 REQUIRED)

file(GLOB_RECURSE fiona_headers CONFIGURE_DEPENDS "include/*/*.hpp")
file(GLOB_RECURSE fiona_sources CONFIGURE_DEPENDS "src/**.cpp")

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

if(BUILD_TESTING)
  set_target_properties(fiona PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
  include(CTest)
  include(FionaTest)
  add_subdirectory(test)
endif()
