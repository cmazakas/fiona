find_package(Boost 1.83 REQUIRED)
find_package(Catch2 3.0 REQUIRED)

include(Catch)

function(fiona_bench benchname)
  add_executable(${benchname} ${benchname}.cpp)
    target_link_libraries(
      ${benchname}
      PRIVATE
        fiona
        Boost::headers
        Catch2::Catch2 Catch2::Catch2WithMain
  )
endfunction()

set(fiona_bench_sources
  echo
)

foreach(bench_source ${fiona_bench_sources})
  fiona_bench(${bench_source})
endforeach()
