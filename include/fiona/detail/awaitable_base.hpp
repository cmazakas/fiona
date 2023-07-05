#ifndef FIONA_DETAIL_AWAITABLE_BASE_HPP
#define FIONA_DETAIL_AWAITABLE_BASE_HPP

#include <coroutine>

struct io_uring_cqe;

namespace fiona {
namespace detail {

struct awaitable_base {
  virtual void await_process_cqe( io_uring_cqe* cqe ) = 0;
  virtual std::coroutine_handle<> handle() const noexcept = 0;
};

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_AWAITABLE_BASE_HPP
