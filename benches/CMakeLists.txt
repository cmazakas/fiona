find_package(Boost 1.83 REQUIRED)
find_package(Catch2 3.0 REQUIRED)

include(Catch)

add_executable(echo echo/common.hpp echo/echo.cpp echo/asio.cpp echo/fiona.cpp)
target_link_libraries(
  echo
  PRIVATE
    fiona
    Boost::headers
    Catch2::Catch2 Catch2::Catch2WithMain
)
