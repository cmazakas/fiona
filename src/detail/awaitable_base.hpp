// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_DETAIL_AWAITABLE_BASE_HPP
#define FIONA_DETAIL_AWAITABLE_BASE_HPP

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <coroutine>

#include <fiona_export.h>

struct io_uring_cqe;

namespace fiona {
namespace detail {

struct FIONA_EXPORT ref_count
{
  int count_ = 0;
  virtual ~ref_count();

  int
  use_count() const noexcept
  {
    return count_;
  }
};

inline void
intrusive_ptr_add_ref( ref_count* prc ) noexcept
{
  ++prc->count_;
}

inline void
intrusive_ptr_release( ref_count* p_rc )
{
  if ( --p_rc->count_ == 0 ) {
    delete p_rc;
  }
}

struct FIONA_EXPORT awaitable_base : public virtual ref_count
{
  virtual ~awaitable_base() override;
  virtual void await_process_cqe( io_uring_cqe* cqe ) = 0;
  virtual std::coroutine_handle<> handle() noexcept = 0;
};

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_AWAITABLE_BASE_HPP
