find_package(Catch2 3.0 REQUIRED)

include(Catch)

add_test(
  NAME "fiona-verify-headers"
  COMMAND "${CMAKE_COMMAND}" --build ${CMAKE_BINARY_DIR}
                             --target "fiona_verify_interface_header_sets"
                             --config $<CONFIG
)

function(fiona_test testname)
  add_executable(${testname} helpers.hpp ${testname}.cpp)
  target_link_libraries(
    ${testname}
    PRIVATE
      Fiona::core
      Boost::config
      Catch2::Catch2 Catch2::Catch2WithMain
  )
  catch_discover_tests(${testname} TEST_PREFIX "${testname}:")
endfunction()

function(fiona_tls_test testname)
  add_executable(${testname} helpers.hpp tls/${testname}.cpp)
  target_link_libraries(
    ${testname}
    PRIVATE
      Fiona::core
      Fiona::tls
      Boost::config
      Catch2::Catch2 Catch2::Catch2WithMain
  )
  catch_discover_tests(
    ${testname}
    TEST_PREFIX "${testname}:"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/tls/botan"
  )
endfunction()

file(
  GLOB fiona_test_sources
  CONFIGURE_DEPENDS
  "*_test.cpp"
)

foreach(test_source ${fiona_test_sources})
  cmake_path(SET test_path ${test_source})
  cmake_path(GET test_path STEM test_filename)
  fiona_test(${test_filename})
endforeach()

if(Botan_FOUND)
  file(
    GLOB fiona_tls_test_sources
    CONFIGURE_DEPENDS
    "tls/*_test.cpp"
  )

  foreach(test_source ${fiona_tls_test_sources})
    cmake_path(SET test_path ${test_source})
    cmake_path(GET test_path STEM test_filename)
    fiona_tls_test(${test_filename})
  endforeach()
endif()
