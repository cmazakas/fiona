#ifndef FIONA_DETAIL_AWAITABLE_BASE_HPP
#define FIONA_DETAIL_AWAITABLE_BASE_HPP

#include <boost/smart_ptr/intrusive_ref_counter.hpp>

#include <coroutine>

struct io_uring_cqe;

namespace fiona {
namespace detail {

struct awaitable_base
    : public boost::intrusive_ref_counter<awaitable_base,
                                          boost::thread_unsafe_counter> {
  virtual ~awaitable_base() {}
  virtual void await_process_cqe( io_uring_cqe* cqe ) = 0;
  virtual std::coroutine_handle<> handle() noexcept = 0;
};

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_AWAITABLE_BASE_HPP
