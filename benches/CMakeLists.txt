find_package(Boost 1.83 REQUIRED)
find_package(Catch2 3.0 REQUIRED)

include(Catch)

function(add_bench_deps tgt)
  target_link_libraries(
    ${tgt}
    PRIVATE
      fiona
      Boost::headers
      Catch2::Catch2 Catch2::Catch2WithMain)
endfunction()

add_executable(
  echo echo/common.hpp echo/echo.cpp echo/asio.cpp echo/fiona.cpp)
add_bench_deps(
  echo)

add_executable(
  timer timer/common.hpp timer/timer.cpp timer/asio.cpp timer/fiona.cpp)
add_bench_deps(
  timer)

add_executable(
  post post/common.hpp post/post.cpp post/asio.cpp post/fiona.cpp)
add_bench_deps(
  post)
