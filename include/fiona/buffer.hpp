#ifndef FIONA_BUFFER_HPP
#define FIONA_BUFFER_HPP

#include <fiona/detail/config.hpp>

#include <boost/assert.hpp>
#include <boost/core/exchange.hpp>

#include <cstddef>
#include <iostream>
#include <iterator>
#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace fiona {
struct recv_buffer;

namespace detail {

struct buf_header_impl {
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  unsigned char* prev_ = nullptr;
  unsigned char* next_ = nullptr;
};

inline constexpr buf_header_impl const default_buf_header_;

inline unsigned char*
default_buf_header() {
  return reinterpret_cast<unsigned char*>(
      const_cast<detail::buf_header_impl*>( &default_buf_header_ ) );
}

} // namespace detail

struct recv_buffer_view {
protected:
  friend struct recv_buffer_sequence;
  friend struct recv_buffer_sequence_view;

  constexpr static std::size_t const S = sizeof( detail::buf_header_impl );

  unsigned char* p_ = detail::default_buf_header();

  detail::buf_header_impl& header() noexcept {
    return *reinterpret_cast<detail::buf_header_impl*>( p_ );
  }
  detail::buf_header_impl const& header() const noexcept {
    return *reinterpret_cast<detail::buf_header_impl*>( p_ );
  }

  recv_buffer_view( unsigned char* p ) noexcept : p_( p ) {}

public:
  recv_buffer_view() = default;
  recv_buffer_view( recv_buffer_view const& ) = default;
  recv_buffer_view& operator=( recv_buffer_view const& ) = default;

  recv_buffer_view( recv_buffer_view&& ) = default;
  recv_buffer_view& operator=( recv_buffer_view&& ) = default;

  ~recv_buffer_view() = default;

  unsigned char* data() noexcept { return p_ + S; }
  unsigned char const* data() const noexcept { return p_ + S; }
  std::size_t size() const noexcept { return header().size_; }
  std::size_t capacity() const noexcept { return header().capacity_; }
  bool empty() const noexcept { return size() == 0; }

  void set_len( std::size_t size ) noexcept {
    BOOST_ASSERT( capacity() > 0 );
    header().size_ = size;
  }

  // todo: rename this
  std::span<unsigned char> readable_bytes() noexcept {
    return { data(), size() };
  }

  std::string_view as_str() const noexcept {
    return { reinterpret_cast<char const*>( data() ), size() };
  }

  std::span<unsigned char> spare_capacity_mut() noexcept {
    return { data() + size(), capacity() - size() };
  }
};

struct recv_buffer : public recv_buffer_view {
private:
  friend struct recv_buffer_sequence;
  friend struct recv_buffer_sequence_view;

  constexpr static std::align_val_t const A{
      alignof( detail::buf_header_impl ) };

  static void* aligned_alloc( std::size_t n ) { return ::operator new( n, A ); }
  static void aligned_delete( void* p ) { ::operator delete( p, A ); }

  unsigned char* to_raw_parts() noexcept {
    auto p = p_;
    p_ = detail::default_buf_header();
    return p;
  }

public:
  recv_buffer() = default;
  recv_buffer( std::size_t capacity )
      : recv_buffer_view(
            static_cast<unsigned char*>( aligned_alloc( S + capacity ) ) ) {
    new ( p_ ) detail::buf_header_impl();
    header().capacity_ = capacity;
  }

  recv_buffer( recv_buffer const& ) = delete;
  recv_buffer& operator=( recv_buffer const& ) = delete;

  recv_buffer( recv_buffer&& rhs ) noexcept
      : recv_buffer_view(
            boost::exchange( rhs.p_, detail::default_buf_header() ) ) {}

  recv_buffer& operator=( recv_buffer&& rhs ) noexcept {
    if ( this != &rhs ) {
      if ( capacity() > 0 ) {
        header().~buf_header_impl();
        aligned_delete( p_ );
      }
      p_ = rhs.p_;
      rhs.p_ = detail::default_buf_header();
    }
    return *this;
  }

  ~recv_buffer() {
    if ( capacity() > 0 ) {
      aligned_delete( p_ );
    }
  }
};

struct recv_buffer_sequence_view {
protected:
  detail::buf_header_impl* psentry_;
  unsigned char* head_ = nullptr;
  unsigned char* tail_ = nullptr;
  std::size_t size_ = 0;

  recv_buffer_sequence_view( detail::buf_header_impl* psentry )
      : psentry_( psentry ) {}

public:
  recv_buffer_sequence_view() = delete;

  recv_buffer_sequence_view( recv_buffer_sequence_view const& ) = default;
  recv_buffer_sequence_view&
  operator=( recv_buffer_sequence_view const& ) = default;

  recv_buffer_sequence_view( recv_buffer_sequence_view&& ) = default;
  recv_buffer_sequence_view& operator=( recv_buffer_sequence_view&& ) = default;

  struct iterator {
  private:
    friend struct recv_buffer_sequence_view;

    detail::buf_header_impl* p_ = nullptr;

    iterator( detail::buf_header_impl* p ) : p_( p ) { BOOST_ASSERT( p_ ); }

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

    recv_buffer_view operator*() noexcept {
      return { reinterpret_cast<unsigned char*>( p_ ) };
    }
    recv_buffer_view operator*() const noexcept {
      return { reinterpret_cast<unsigned char*>( p_ ) };
    }

    iterator& operator++() noexcept {
      BOOST_ASSERT( p_ );
      p_ = reinterpret_cast<detail::buf_header_impl*>( p_->next_ );
      return *this;
    }

    iterator operator++( int ) noexcept {
      BOOST_ASSERT( p_ );
      auto const old = p_;
      p_ = reinterpret_cast<detail::buf_header_impl*>( p_->next_ );
      return old;
    }

    iterator& operator--() noexcept {
      p_ = reinterpret_cast<detail::buf_header_impl*>( p_->prev_ );
      return *this;
    }

    iterator operator--( int ) noexcept {
      auto const old = p_;
      p_ = reinterpret_cast<detail::buf_header_impl*>( p_->prev_ );
      return old;
    }

    bool operator==( iterator const& rhs ) const { return p_ == rhs.p_; }
    bool operator!=( iterator const& ) const = default;
  };

  using const_iterator = iterator;

  iterator begin() const {
    return { reinterpret_cast<detail::buf_header_impl*>( head_ ) };
  }

  iterator end() const {
    return { const_cast<detail::buf_header_impl*>( psentry_ ) };
  }

  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size() == 0; }

  FIONA_DECL
  std::string to_string() const;

  FIONA_DECL
  std::vector<unsigned char> to_bytes() const;
};

struct recv_buffer_sequence : public recv_buffer_sequence_view {
private:
  detail::buf_header_impl sentry_;

  void free_list() {
    if ( !head_ ) {
      return;
    }

    auto p = head_;

    while ( p != reinterpret_cast<unsigned char*>( &sentry_ ) ) {
      auto old = p;
      p = recv_buffer_view( p ).header().next_;
      recv_buffer::aligned_delete( old );
    }

    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
    sentry_.next_ = nullptr;
    sentry_.prev_ = nullptr;
  }

public:
  recv_buffer_sequence() : recv_buffer_sequence_view( &sentry_ ) {}

  recv_buffer_sequence( recv_buffer_sequence const& ) = delete;
  recv_buffer_sequence& operator=( recv_buffer_sequence const& ) = delete;

  recv_buffer_sequence( recv_buffer_sequence&& rhs ) noexcept
      : recv_buffer_sequence_view( &sentry_ ) {
    head_ = rhs.head_;
    tail_ = rhs.tail_;
    size_ = rhs.size_;

    if ( tail_ ) {
      recv_buffer_view tail_view( tail_ );
      tail_view.header().next_ = reinterpret_cast<unsigned char*>( psentry_ );
      psentry_->prev_ = tail_;
    }

    if ( head_ ) {
      recv_buffer_view head_view( head_ );
      head_view.header().prev_ = reinterpret_cast<unsigned char*>( psentry_ );
      psentry_->next_ = head_;
    }

    rhs.head_ = nullptr;
    rhs.tail_ = nullptr;
    rhs.size_ = 0;
  }

  recv_buffer_sequence& operator=( recv_buffer_sequence&& rhs ) {
    if ( this != &rhs ) {
      if ( !empty() ) {
        free_list();
      }

      head_ = boost::exchange( rhs.head_, nullptr );
      tail_ = boost::exchange( rhs.tail_, nullptr );
      size_ = boost::exchange( rhs.size_, 0 );

      recv_buffer_view head_view( head_ );
      recv_buffer_view tail_view( tail_ );

      tail_view.header().next_ = reinterpret_cast<unsigned char*>( &sentry_ );
      sentry_.prev_ = tail_;

      head_view.header().prev_ = reinterpret_cast<unsigned char*>( &sentry_ );
      sentry_.next_ = head_;
    }
    return *this;
  }

  ~recv_buffer_sequence() { free_list(); }

  void push_back( recv_buffer rbuf ) noexcept {
    BOOST_ASSERT( rbuf.p_ != detail::default_buf_header() );

    auto p = rbuf.to_raw_parts();
    recv_buffer_view p_view( p );
    if ( tail_ ) {
      recv_buffer_view tail_view( tail_ );
      tail_view.header().next_ = p;
      p_view.header().prev_ = tail_;
    }

    tail_ = p;
    sentry_.prev_ = tail_;
    p_view.header().next_ = reinterpret_cast<unsigned char*>( &sentry_ );

    if ( head_ == nullptr ) {
      head_ = tail_;
      sentry_.next_ = head_;
    }

    ++size_;
  }

  void concat( recv_buffer_sequence rhs ) noexcept {
    if ( rhs.empty() ) {
      return;
    }

    if ( empty() ) {
      *this = std::move( rhs );
      return;
    }

    recv_buffer_view header_view( head_ );
    recv_buffer_view tail_view( tail_ );
    recv_buffer_view rhs_head_view( rhs.head_ );
    recv_buffer_view rhs_tail_view( rhs.tail_ );

    tail_view.header().next_ = rhs.head_;
    rhs_head_view.header().prev_ = tail_;

    rhs_tail_view.header().next_ = reinterpret_cast<unsigned char*>( &sentry_ );
    sentry_.prev_ = rhs.tail_;

    tail_ = rhs.tail_;
    size_ += rhs.size_;

    rhs.head_ = nullptr;
    rhs.tail_ = nullptr;
    rhs.size_ = 0;
  }
};
} // namespace fiona

#endif // FIONA_BUFFER_HPP
