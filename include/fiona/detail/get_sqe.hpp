#ifndef FIONA_DETAIL_GET_SQE_HPP
#define FIONA_DETAIL_GET_SQE_HPP

#include <boost/assert.hpp>

#include <liburing.h>

namespace fiona {
namespace detail {

io_uring_sqe*
get_sqe( io_uring* ring ) {
  auto sqe = io_uring_get_sqe( ring );
  if ( sqe ) {
    return sqe;
  }
  io_uring_submit( ring );
  sqe = io_uring_get_sqe( ring );
  BOOST_ASSERT( sqe );
  return sqe;
}

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_GET_SQE_HPP
