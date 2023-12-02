find_package(Boost 1.83 REQUIRED)
find_package(Catch2 3.0 REQUIRED)

include(Catch)

function(fiona_test testname)
  add_executable(${testname} helpers.hpp ${testname}.cpp)
  target_link_libraries(
    ${testname}
    PRIVATE
      fiona
      Boost::headers
      Catch2::Catch2 Catch2::Catch2WithMain
  )
  catch_discover_tests(${testname})
endfunction()
