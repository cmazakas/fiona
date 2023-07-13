#ifndef FIONA_DETAIL_TIME_HPP
#define FIONA_DETAIL_TIME_HPP

#include <chrono>

#include <liburing.h>

namespace fiona {
namespace detail {

template <class Rep, class Period>
__kernel_timespec
duration_to_timespec( std::chrono::duration<Rep, Period> const& d ) {
  auto sec = std::chrono::floor<std::chrono::seconds>( d );
  auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>( d - sec );
  __kernel_timespec ts;
  ts.tv_sec = sec.count();
  ts.tv_nsec = nsec.count();
  return ts;
}

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_TIME_HPP
