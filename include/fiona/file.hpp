// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_FILE_HPP
#define FIONA_FILE_HPP

#include <fiona/executor.hpp>
#include <fiona_export.h>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <string_view>

#include <fcntl.h>

namespace fiona {
namespace detail {
struct file_impl;

void FIONA_EXPORT intrusive_ptr_add_ref( file_impl* p_file ) noexcept;
void FIONA_EXPORT intrusive_ptr_release( file_impl* p_file ) noexcept;

} // namespace detail

class open_awaitable;

//------------------------------------------------------------------------------

class file
{
  boost::intrusive_ptr<detail::file_impl> p_file_;

public:
  file() = default;

  FIONA_EXPORT
  file( executor ex );

  file( file const& ) = default;
  file& operator=( file const& ) = default;

  file( file&& ) = default;
  file& operator=( file&& ) = default;

  ~file() = default;

  bool operator==( file const& ) const = default;

  FIONA_EXPORT
  open_awaitable async_open( std::string pathname, int flags );
};

//------------------------------------------------------------------------------

class open_awaitable
{
  friend class file;
  boost::intrusive_ptr<detail::file_impl> p_file_;

  open_awaitable( boost::intrusive_ptr<detail::file_impl> p_file )
      : p_file_( p_file )
  {
  }

public:
  open_awaitable() = delete;

  open_awaitable( open_awaitable const& ) = delete;
  open_awaitable& operator=( open_awaitable const& ) = delete;

  bool
  await_ready() const noexcept
  {
    return false;
  }

  FIONA_EXPORT void await_suspend( std::coroutine_handle<> h );

  FIONA_EXPORT result<void> await_resume();
};

} // namespace fiona

#endif // FIONA_FILE_HPP
