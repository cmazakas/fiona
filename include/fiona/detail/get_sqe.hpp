#ifndef FIONA_DETAIL_GET_SQE_HPP
#define FIONA_DETAIL_GET_SQE_HPP

#include <boost/assert.hpp>

#include <liburing.h>

namespace fiona {
namespace detail {

inline void
submit_ring( io_uring* ring )
{
  io_uring_submit_and_get_events( ring );
}

[[nodiscard]] inline io_uring_sqe*
get_sqe( io_uring* ring )
{
  auto sqe = io_uring_get_sqe( ring );
  if ( sqe ) {
    return sqe;
  }
  submit_ring( ring );
  sqe = io_uring_get_sqe( ring );
  BOOST_ASSERT( sqe );
  return sqe;
}

inline void
reserve_sqes( io_uring* ring, unsigned n )
{
  auto r = io_uring_sq_space_left( ring );
  if ( r < n ) {
    submit_ring( ring );
  }
}

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_GET_SQE_HPP
