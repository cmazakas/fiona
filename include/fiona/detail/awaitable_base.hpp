#ifndef FIONA_DETAIL_AWAITABLE_BASE_HPP
#define FIONA_DETAIL_AWAITABLE_BASE_HPP

#include <coroutine>

struct io_uring_cqe;

namespace fiona {
namespace detail {

struct awaitable_base {
  virtual ~awaitable_base() {}
  virtual void await_process_cqe( io_uring_cqe* cqe ) = 0;
  virtual std::coroutine_handle<> handle() noexcept = 0;
  virtual void inc_ref() noexcept = 0;
  virtual void dec_ref() noexcept = 0;
  virtual int use_count() const noexcept = 0;
};

inline void
intrusive_ptr_add_ref( awaitable_base* ab ) {
  ab->inc_ref();
}

inline void
intrusive_ptr_release( awaitable_base* ab ) {
  ab->dec_ref();
}

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_AWAITABLE_BASE_HPP
