cmake_minimum_required(VERSION 3.25)
project(fiona_subdir_test LANGUAGES CXX)

add_subdirectory(../.. fiona)

add_executable(main main.cpp)
target_link_libraries(main PRIVATE Fiona::fiona)

include(CTest)
add_test(NAME main COMMAND main)
