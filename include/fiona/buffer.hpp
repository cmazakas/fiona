// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_BUFFER_HPP
#define FIONA_BUFFER_HPP

#include <boost/assert.hpp> // for BOOST_ASSERT
#include <boost/config.hpp>

#include <cstddef>          // for size_t, ptrdiff_t
#include <iterator>         // for bidirectional_iterator_tag
#include <new>              // for align_val_t, operator new
#include <span>             // for span
#include <string>           // for string
#include <string_view>      // for string_view
#include <utility>          // for move
#include <vector>           // for vector

#include <fiona_export.h>   // for FIONA_EXPORT

namespace fiona {

class recv_buffer;

namespace detail {

struct buf_header_impl
{
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  unsigned char* prev_ = nullptr;
  unsigned char* next_ = nullptr;
};

FIONA_EXPORT unsigned char* default_buf_header();

inline void
set_next( unsigned char* buf, unsigned char* next ) noexcept
{
  reinterpret_cast<detail::buf_header_impl*>( buf )->next_ = next;
}

inline void
set_next( unsigned char* buf, detail::buf_header_impl* header ) noexcept
{
  reinterpret_cast<detail::buf_header_impl*>( buf )->next_ =
      reinterpret_cast<unsigned char*>( header );
}

inline void
set_prev( unsigned char* buf, unsigned char* prev ) noexcept
{
  reinterpret_cast<detail::buf_header_impl*>( buf )->prev_ = prev;
}

inline void
set_prev( detail::buf_header_impl& header, unsigned char* prev ) noexcept
{
  header.prev_ = prev;
}

inline unsigned char*
get_next( unsigned char* buf ) noexcept
{
  return reinterpret_cast<detail::buf_header_impl*>( buf )->next_;
}

inline unsigned char*
get_prev( unsigned char* buf ) noexcept
{
  return reinterpret_cast<detail::buf_header_impl*>( buf )->prev_;
}

} // namespace detail

class recv_buffer_view
{
  friend class recv_buffer;
  friend class recv_buffer_sequence;
  friend class recv_buffer_sequence_view;

  constexpr static std::size_t const S = sizeof( detail::buf_header_impl );

  unsigned char* p_ = detail::default_buf_header();

  detail::buf_header_impl&
  header() noexcept
  {
    return *reinterpret_cast<detail::buf_header_impl*>( p_ );
  }

  detail::buf_header_impl const&
  header() const noexcept
  {
    return *reinterpret_cast<detail::buf_header_impl*>( p_ );
  }

  bool
  is_defaulted() const noexcept
  {
    return p_ == detail::default_buf_header();
  }

  recv_buffer_view( unsigned char* p ) noexcept : p_( p ) {}

public:
  recv_buffer_view() = default;

  recv_buffer_view( recv_buffer_view const& ) = default;
  recv_buffer_view& operator=( recv_buffer_view const& ) = default;

  recv_buffer_view( recv_buffer_view&& ) = default;
  recv_buffer_view& operator=( recv_buffer_view&& ) = default;

  ~recv_buffer_view() = default;

  unsigned char*
  data() noexcept
  {
    return p_ + S;
  }

  unsigned char const*
  data() const noexcept
  {
    return p_ + S;
  }

  std::size_t
  size() const noexcept
  {
    return header().size_;
  }

  std::size_t
  capacity() const noexcept
  {
    return header().capacity_;
  }

  bool
  empty() const noexcept
  {
    return size() == 0;
  }

  void
  set_len( std::size_t size ) noexcept
  {
    BOOST_ASSERT( !is_defaulted() );
    header().size_ = size;
  }

  // todo: rename this
  std::span<unsigned char>
  readable_bytes() noexcept
  {
    return { data(), size() };
  }

  std::string_view
  as_str() const noexcept
  {
    return { reinterpret_cast<char const*>( data() ), size() };
  }

  std::span<unsigned char>
  spare_capacity_mut() noexcept
  {
    return { data() + size(), capacity() - size() };
  }
};

class recv_buffer : public recv_buffer_view
{
  friend class recv_buffer_sequence;
  friend class recv_buffer_sequence_view;

  constexpr static std::align_val_t const A{
      alignof( detail::buf_header_impl ) };

  static void*
  aligned_alloc( std::size_t n )
  {
    return ::operator new( n, A );
  }

  static void
  aligned_delete( void* p )
  {
    ::operator delete( p, A );
  }

  unsigned char*
  to_raw_parts() noexcept
  {
    auto p = p_;
    p_ = detail::default_buf_header();
    return p;
  }

public:
  recv_buffer() = default;
  recv_buffer( std::size_t capacity )
      : recv_buffer_view(
            static_cast<unsigned char*>( aligned_alloc( S + capacity ) ) )
  {
    new ( p_ ) detail::buf_header_impl();
    header().capacity_ = capacity;
  }

  recv_buffer( recv_buffer const& ) = delete;
  recv_buffer& operator=( recv_buffer const& ) = delete;

  recv_buffer( recv_buffer&& rhs ) noexcept : recv_buffer_view( rhs.p_ )
  {
    rhs.p_ = detail::default_buf_header();
  }

  recv_buffer&
  operator=( recv_buffer&& rhs ) noexcept
  {
    if ( this != &rhs ) {
      if ( !is_defaulted() ) {
        header().~buf_header_impl();
        aligned_delete( p_ );
      }
      p_ = rhs.p_;
      rhs.p_ = detail::default_buf_header();
    }
    return *this;
  }

  ~recv_buffer()
  {
    if ( !is_defaulted() ) {
      aligned_delete( p_ );
    }
  }
};

class recv_buffer_sequence_view
{
  friend class recv_buffer_sequence;

  detail::buf_header_impl* p_sentry_;
  unsigned char* head_ = nullptr;
  unsigned char* tail_ = nullptr;
  std::size_t num_bufs_ = 0;

  recv_buffer_sequence_view( detail::buf_header_impl* p_sentry )
      : p_sentry_( p_sentry )
  {
  }

public:
  recv_buffer_sequence_view() = delete;

  recv_buffer_sequence_view( recv_buffer_sequence_view const& ) = default;
  recv_buffer_sequence_view&
  operator=( recv_buffer_sequence_view const& ) = default;

  recv_buffer_sequence_view( recv_buffer_sequence_view&& ) = default;
  recv_buffer_sequence_view& operator=( recv_buffer_sequence_view&& ) = default;

  class iterator
  {
    friend class recv_buffer_sequence_view;

    unsigned char* p_ = nullptr;

    iterator( unsigned char* p ) : p_( p ) { BOOST_ASSERT( p_ ); }

  public:
    using value_type = recv_buffer_view;
    using difference_type = std::ptrdiff_t;
    using reference = value_type;
    using pointer = value_type*;
    using iterator_category = std::bidirectional_iterator_tag;

    iterator() = default;
    ~iterator() = default;
    iterator( iterator const& ) = default;
    iterator& operator=( iterator const& ) = default;

    iterator( iterator&& ) = default;
    iterator& operator=( iterator&& ) = default;

    recv_buffer_view
    operator*() noexcept
    {
      return { p_ };
    }

    recv_buffer_view
    operator*() const noexcept
    {
      return { p_ };
    }

    iterator&
    operator++() noexcept
    {
      BOOST_ASSERT( p_ );
      p_ = detail::get_next( p_ );
      return *this;
    }

    iterator
    operator++( int ) noexcept
    {
      BOOST_ASSERT( p_ );
      auto const old = p_;
      p_ = detail::get_next( p_ );
      return old;
    }

    iterator&
    operator--() noexcept
    {
      p_ = detail::get_prev( p_ );
      return *this;
    }

    iterator
    operator--( int ) noexcept
    {
      auto const old = p_;
      p_ = detail::get_prev( p_ );
      return old;
    }

    bool
    operator==( iterator const& rhs ) const
    {
      return p_ == rhs.p_;
    }

    bool operator!=( iterator const& ) const = default;
  };

  using const_iterator = iterator;

  iterator
  begin() const
  {
    if ( empty() ) {
      return end();
    }
    return { head_ };
  }

  iterator
  end() const
  {
    return { reinterpret_cast<unsigned char*>(
        const_cast<detail::buf_header_impl*>( p_sentry_ ) ) };
  }

  std::size_t
  num_bufs() const noexcept
  {
    return num_bufs_;
  }

  bool
  empty() const noexcept
  {
    return num_bufs() == 0;
  }

  recv_buffer_view
  front() const noexcept
  {
    BOOST_ASSERT( !empty() );
    return recv_buffer_view( head_ );
  }

  recv_buffer_view
  back() const noexcept
  {
    BOOST_ASSERT( !empty() );
    return recv_buffer_view( tail_ );
  }

  FIONA_EXPORT
  std::string to_string() const;

  FIONA_EXPORT
  std::vector<unsigned char> to_bytes() const;
};

class recv_buffer_sequence : public recv_buffer_sequence_view
{
  detail::buf_header_impl sentry_;

  void
  free_list()
  {
    if ( !head_ ) {
      return;
    }

    auto p = head_;

    while ( p != reinterpret_cast<unsigned char*>( &sentry_ ) ) {
      auto old = p;
      p = detail::get_next( p );
      recv_buffer::aligned_delete( old );
    }

    head_ = nullptr;
    tail_ = nullptr;
    num_bufs_ = 0;
    sentry_.next_ = nullptr;
    sentry_.prev_ = nullptr;
  }

public:
  recv_buffer_sequence() : recv_buffer_sequence_view( &sentry_ ) {}

  recv_buffer_sequence( recv_buffer_sequence const& ) = delete;
  recv_buffer_sequence& operator=( recv_buffer_sequence const& ) = delete;

  recv_buffer_sequence( recv_buffer_sequence&& rhs ) noexcept
      : recv_buffer_sequence_view( &sentry_ )
  {
    head_ = rhs.head_;
    tail_ = rhs.tail_;
    num_bufs_ = rhs.num_bufs_;

    rhs.head_ = nullptr;
    rhs.tail_ = nullptr;
    rhs.num_bufs_ = 0;

    set_prev( rhs.sentry_, nullptr );
    set_prev( sentry_, tail_ );
    if ( tail_ ) {
      set_next( tail_, &sentry_ );
    }
  }

  recv_buffer_sequence&
  operator=( recv_buffer_sequence&& rhs )
  {
    if ( this != &rhs ) {
      if ( !empty() ) {
        free_list();
      }

      head_ = rhs.head_;
      tail_ = rhs.tail_;
      num_bufs_ = rhs.num_bufs_;

      rhs.head_ = nullptr;
      rhs.tail_ = nullptr;
      rhs.num_bufs_ = 0;

      set_prev( rhs.sentry_, nullptr );
      set_prev( sentry_, tail_ );
      if ( tail_ ) {
        set_next( tail_, &sentry_ );
      }
    }
    return *this;
  }

  ~recv_buffer_sequence() { free_list(); }

  void
  push_back( recv_buffer rbuf ) noexcept
  {
    BOOST_ASSERT( rbuf.p_ != detail::default_buf_header() );

    auto p = rbuf.to_raw_parts();

    if ( BOOST_LIKELY( tail_ != nullptr ) ) {
      detail::set_next( tail_, p );
      detail::set_prev( p, tail_ );
    }

    set_next( p, &sentry_ );
    set_prev( sentry_, p );
    tail_ = p;
    if ( BOOST_UNLIKELY( head_ == nullptr ) ) {
      head_ = p;
    }

    ++num_bufs_;
  }

  recv_buffer
  pop_front()
  {
    BOOST_ASSERT( !empty() );

    recv_buffer buf;

    auto p = head_;
    head_ = detail::get_next( head_ );

    detail::set_next( p, static_cast<unsigned char*>( nullptr ) );
    detail::set_prev( p, static_cast<unsigned char*>( nullptr ) );

    // this is unconditionally valid, even when `head_ == tail_`
    // because `tail_->next_ == &sentry_`
    detail::set_prev( head_, nullptr );

    --num_bufs_;
    buf.p_ = p;

    // old head value was the tail, meaning our list is now empty
    if ( BOOST_UNLIKELY( p == tail_ ) ) {
      BOOST_ASSERT( num_bufs_ == 0 );

      tail_ = nullptr;
      head_ = nullptr;
    }

    return buf;
  }

  void
  concat( recv_buffer_sequence rhs ) noexcept
  {
    if ( rhs.empty() ) {
      return;
    }

    if ( empty() ) {
      *this = std::move( rhs );
      return;
    }

    detail::set_next( tail_, rhs.head_ );
    detail::set_prev( rhs.head_, tail_ );

    detail::set_next( rhs.tail_, &sentry_ );
    detail::set_prev( sentry_, rhs.tail_ );

    detail::set_prev( rhs.sentry_, nullptr );

    tail_ = rhs.tail_;
    num_bufs_ += rhs.num_bufs_;

    rhs.head_ = nullptr;
    rhs.tail_ = nullptr;
    rhs.num_bufs_ = 0;
  }
};
} // namespace fiona

#endif // FIONA_BUFFER_HPP
