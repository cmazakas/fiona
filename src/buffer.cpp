// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <fiona/buffer.hpp>

#include <algorithm> // for min
#include <cstring>   // for memcpy
#include <span>      // for span
#include <string>    // for string

namespace fiona {
namespace detail {

inline constexpr detail::buf_header_impl const default_buf_header_;

unsigned char*
default_buf_header()
{
  return reinterpret_cast<unsigned char*>(
      const_cast<detail::buf_header_impl*>( &default_buf_header_ ) );
}

} // namespace detail

namespace {

std::size_t
buffer_size( recv_buffer_sequence_view bufs )
{
  std::size_t n = 0;
  for ( auto buf_view : bufs ) {
    n += buf_view.size();
  }
  return n;
}

std::size_t
buffer_copy( std::span<unsigned char> dst, recv_buffer_sequence_view bufs )
{
  std::size_t n = 0;

  auto end = bufs.end();
  for ( auto pos = bufs.begin(); pos != end; ++pos ) {
    auto buf_view = *pos;
    if ( buf_view.empty() ) {
      continue;
    }

    auto m = std::min( dst.size(), buf_view.size() );
    std::memcpy( dst.data(), buf_view.data(), m );
    n += m;

    if ( m < buf_view.size() ) {
      break;
    }

    dst = dst.subspan( m );
    if ( dst.empty() ) {
      break;
    }
  }
  return n;
}

std::size_t
buffer_copy( std::span<char> dst, recv_buffer_sequence_view bufs )
{
  return buffer_copy(
      std::span( reinterpret_cast<unsigned char*>( dst.data() ), dst.size() ),
      bufs );
}

} // namespace

std::string
recv_buffer_sequence_view::to_string() const
{
  auto n = buffer_size( *this );
  std::string s( n, '\0' );
  buffer_copy( std::span( s.data(), s.size() ), *this );
  return s;
}

std::vector<unsigned char>
recv_buffer_sequence_view::to_bytes() const
{
  auto n = buffer_size( *this );
  std::vector<unsigned char> s( n, 0x00 );
  buffer_copy( s, *this );
  return s;
}

} // namespace fiona
