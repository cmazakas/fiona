// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <fiona/file.hpp>

#include <fiona/detail/get_sqe.hpp>

#include "detail/awaitable_base.hpp"

namespace fiona {
namespace detail {

struct file_impl;

struct open_frame : public awaitable_base
{
  std::coroutine_handle<> h_ = nullptr;

  int temp_fd_ = -1;
  std::string pathname_;
  int flags_ = -1;

  int res_ = 0;
  bool initiated_ = false;
  bool done_ = false;

  open_frame() = default;

  open_frame( open_frame const& ) = delete;
  open_frame& operator=( open_frame const& ) = delete;

  ~open_frame() override;

  void
  reset()
  {
    h_ = nullptr;
    temp_fd_ = -1;
    res_ = 0;
    initiated_ = false;
    done_ = false;
  }

  void await_process_cqe( io_uring_cqe* cqe ) override;

  std::coroutine_handle<>
  handle() noexcept override
  {
    return boost::exchange( h_, nullptr );
  }
};

open_frame::~open_frame() = default;

//------------------------------------------------------------------------------

struct file_impl final : public virtual ref_count, public open_frame
{
  executor ex_;
  int fd_ = -1;

  file_impl( executor ex ) : ex_( ex ) {}

  ~file_impl() override;
};

file_impl::~file_impl()
{
  if ( fd_ >= 0 ) {
    auto ring = fiona::detail::executor_access_policy::ring( ex_ );
    auto sqe = fiona::detail::get_sqe( ring );

    io_uring_prep_close_direct( sqe, static_cast<unsigned>( fd_ ) );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    fiona::detail::submit_ring( ring );
    fiona::detail::executor_access_policy::release_fd( ex_, fd_ );
    fd_ = -1;
  }
}

void FIONA_EXPORT
intrusive_ptr_add_ref( file_impl* p_file ) noexcept
{
  intrusive_ptr_add_ref( static_cast<ref_count*>( p_file ) );
}

void FIONA_EXPORT
intrusive_ptr_release( file_impl* p_file ) noexcept
{
  intrusive_ptr_release( static_cast<ref_count*>( p_file ) );
}

} // namespace detail

file::file( executor ex ) : p_file_( new detail::file_impl( ex ) ) {}

open_awaitable
file::async_open( std::string pathname, int flags )
{
  auto& of = static_cast<detail::open_frame&>( *p_file_ );
  of.pathname_ = std::move( pathname );
  of.flags_ = flags;

  return { p_file_ };
}

//------------------------------------------------------------------------------

void
detail::open_frame::await_process_cqe( io_uring_cqe* cqe )
{
  auto& file_ = static_cast<detail::file_impl&>( *this );

  done_ = true;
  res_ = cqe->res;

  if ( res_ >= 0 ) {
    file_.fd_ = temp_fd_;
  } else {
    BOOST_ASSERT( temp_fd_ >= 0 );

    auto ex = file_.ex_;
    fiona::detail::executor_access_policy::release_fd( ex, temp_fd_ );
  }

  temp_fd_ = -1;
}

//------------------------------------------------------------------------------

void
open_awaitable::await_suspend( std::coroutine_handle<> h )
{
  auto& of = static_cast<detail::open_frame&>( *p_file_ );
  auto ex = p_file_->ex_;
  auto ring = detail::executor_access_policy::ring( ex );

  detail::reserve_sqes( ring, 1 );

  auto file_index = detail::executor_access_policy::get_available_fd( ex );

  auto sqe = io_uring_get_sqe( ring );
  io_uring_prep_openat_direct( sqe, AT_FDCWD, of.pathname_.c_str(), of.flags_,
                               0, static_cast<unsigned>( file_index ) );
  io_uring_sqe_set_data( sqe,
                         static_cast<detail::open_frame*>( p_file_.get() ) );

  intrusive_ptr_add_ref( &of );

  of.initiated_ = true;
  of.h_ = h;
  of.temp_fd_ = file_index;
}

result<void>
open_awaitable::await_resume()
{
  auto& of = static_cast<detail::open_frame&>( *p_file_ );
  auto res = of.res_;
  of.reset();

  if ( res == 0 ) {
    return {};
  }

  return { error_code::from_errno( -res ) };
}

} // namespace fiona