find_package(Boost 1.82 REQUIRED )

function(fiona_test testname)
  add_executable(${testname} ${testname}.cpp)
  target_link_libraries(${testname} PRIVATE fiona Boost::headers)
  add_test(
    NAME "fiona-${testname}" COMMAND ${testname})
endfunction()
