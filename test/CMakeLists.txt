set(fiona_test_sources

  test/buffer_test.cpp
  test/timer_test.cpp
  test/ipv6_test.cpp
  test/recv_test.cpp
  test/waker_test.cpp
  test/post_test.cpp
  test/tcp_test.cpp
  test/dns_test.cpp
)

set(fiona_tls_test_sources

  test/tls_test.cpp
)

foreach(test_source ${fiona_test_sources})
  cmake_path(SET test_path ${test_source})
  cmake_path(GET test_path STEM test_filename)
  fiona_test(${test_filename})
endforeach()

foreach(test_source ${fiona_tls_test_sources})
  cmake_path(SET test_path ${test_source})
  cmake_path(GET test_path STEM test_filename)
  fiona_tls_test(${test_filename})
endforeach()
