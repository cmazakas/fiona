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
class write_awaitable;
class read_awaitable;

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

  FIONA_EXPORT open_awaitable async_open( std::string pathname, int flags );

  FIONA_EXPORT
  write_awaitable async_write( std::span<unsigned char const> msg );
  FIONA_EXPORT write_awaitable async_write( std::string_view msg );

  FIONA_EXPORT write_awaitable async_write_fixed( fixed_buffer const& buf );

  FIONA_EXPORT read_awaitable async_read( std::span<char> buf );
  FIONA_EXPORT read_awaitable async_read( std::span<unsigned char> buf );
  FIONA_EXPORT read_awaitable async_read_fixed( fixed_buffer& buf );
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

  // TODO: must implement cancel-on-drop here
  FIONA_EXPORT ~open_awaitable();

  bool
  await_ready() const noexcept
  {
    return false;
  }

  FIONA_EXPORT void await_suspend( std::coroutine_handle<> h );
  FIONA_EXPORT result<void> await_resume();
};

//------------------------------------------------------------------------------

class write_awaitable
{
  friend class file;
  boost::intrusive_ptr<detail::file_impl> p_file_;

  write_awaitable( boost::intrusive_ptr<detail::file_impl> p_file )
      : p_file_( p_file )
  {
  }

public:
  write_awaitable() = delete;

  write_awaitable( write_awaitable const& ) = delete;
  write_awaitable& operator=( write_awaitable const& ) = delete;

  // TODO: must implement cancel-on-drop here
  FIONA_EXPORT ~write_awaitable();

  bool
  await_ready() const noexcept
  {
    return false;
  }

  FIONA_EXPORT void await_suspend( std::coroutine_handle<> h );
  FIONA_EXPORT result<std::size_t> await_resume();
};

//------------------------------------------------------------------------------

class read_awaitable
{
  friend class file;
  boost::intrusive_ptr<detail::file_impl> p_file_;

  read_awaitable( boost::intrusive_ptr<detail::file_impl> p_file )
      : p_file_( p_file )
  {
  }

public:
  read_awaitable() = delete;

  read_awaitable( read_awaitable const& ) = delete;
  read_awaitable& operator=( read_awaitable const& ) = delete;

  // TODO: must implement cancel-on-drop here
  FIONA_EXPORT ~read_awaitable();

  bool
  await_ready() const noexcept
  {
    return false;
  }

  FIONA_EXPORT void await_suspend( std::coroutine_handle<> h );
  FIONA_EXPORT result<std::size_t> await_resume();
};

} // namespace fiona

#endif // FIONA_FILE_HPP
