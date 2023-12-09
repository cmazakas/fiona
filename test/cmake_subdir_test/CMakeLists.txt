cmake_minimum_required(VERSION 3.25)
project(fiona_subdir_test LANGUAGES CXX)

add_subdirectory(../../ fiona EXCLUDE_FROM_ALL)

add_executable(main main.cpp)
target_link_libraries(main PRIVATE Fiona::fiona)
