#ifndef FIONA_DETAIL_COMMON_HPP
#define FIONA_DETAIL_COMMON_HPP

#include <fiona/params.hpp> // for io_context_params

#include <fiona/detail/config.hpp>

#include <boost/container_hash/hash.hpp>          // for hash
#include <boost/core/exchange.hpp>
#include <boost/unordered/unordered_flat_map.hpp> // for unordered_flat_map
#include <boost/unordered/unordered_flat_set.hpp> // for unordered_flat_set

#include <coroutine>                              // for coroutine_handle
#include <cstdint>                                // for uint16_t
#include <cstring>                                // for size_t
#include <deque>                                  // for deque
#include <exception>                              // for exception_ptr
#include <list>
#include <mutex>                                  // for mutex
#include <new>
#include <span>                                   // for span
#include <vector>                                 // for vector

#include <liburing.h>                             // for io_uring

struct io_uring_buf_ring;

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
  return reinterpret_cast<unsigned char*>( const_cast<detail::buf_header_impl*>( &default_buf_header_ ) );
}

} // namespace detail

struct recv_buffer_view {
protected:
  friend struct recv_buffer_sequence;
  friend struct recv_buffer_sequence_view;

  constexpr static std::size_t const S = sizeof( detail::buf_header_impl );

  unsigned char* p_ = detail::default_buf_header();

  detail::buf_header_impl& header() noexcept { return *reinterpret_cast<detail::buf_header_impl*>( p_ ); }
  detail::buf_header_impl const& header() const noexcept { return *reinterpret_cast<detail::buf_header_impl*>( p_ ); }

  recv_buffer_view( unsigned char* p ) : p_( p ) {}

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

  void set_len( std::size_t size ) noexcept { header().size_ = size; }

  // todo: rename this
  std::span<unsigned char> readable_bytes() noexcept { return { data(), size() }; }
  std::string_view as_str() const noexcept { return { reinterpret_cast<char const*>( data() ), size() }; }
  std::span<unsigned char> spare_capacity_mut() noexcept { return { data() + size(), capacity() - size() }; }
};

struct recv_buffer : public recv_buffer_view {
private:
  friend struct recv_buffer_sequence;
  friend struct recv_buffer_sequence_view;

  constexpr static std::align_val_t const A{ alignof( detail::buf_header_impl ) };

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
      : recv_buffer_view( static_cast<unsigned char*>( aligned_alloc( S + capacity ) ) ) {
    new ( p_ ) detail::buf_header_impl();
    header().capacity_ = capacity;
  }

  recv_buffer( recv_buffer const& ) = delete;
  recv_buffer& operator=( recv_buffer const& ) = delete;

  recv_buffer( recv_buffer&& rhs ) noexcept
      : recv_buffer_view( boost::exchange( rhs.p_, detail::default_buf_header() ) ) {}

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
  unsigned char* head_ = nullptr;
  unsigned char* tail_ = nullptr;

public:
  using value_type = recv_buffer_view;

  recv_buffer_sequence_view() = default;

  recv_buffer_sequence_view( recv_buffer_sequence_view const& ) = default;
  recv_buffer_sequence_view& operator=( recv_buffer_sequence_view const& ) = default;

  recv_buffer_sequence_view( recv_buffer_sequence_view&& ) = default;
  recv_buffer_sequence_view& operator=( recv_buffer_sequence_view&& ) = default;

  struct iterator {
  private:
    friend struct recv_buffer_sequence_view;

    unsigned char* p_ = nullptr;

    iterator( unsigned char* p ) : p_( p ) {}

  public:
    using value_type = recv_buffer_view;

    iterator() = default;
    ~iterator() = default;

    recv_buffer_view operator*() noexcept { return { p_ }; }

    iterator& operator++() noexcept {
      BOOST_ASSERT( p_ );
      recv_buffer_view v( p_ );
      auto next = v.header().next_;
      p_ = next;
      return *this;
    }

    iterator& operator--() noexcept {
      BOOST_ASSERT( p_ );
      recv_buffer_view v( p_ );
      auto next = v.header().prev_;
      p_ = next;
      return *this;
    }

    bool operator==( iterator const& ) const = default;
  };

  using const_iterator = iterator;

  iterator begin() const { return { head_ }; }
  iterator end() const { return {}; }
};

struct recv_buffer_sequence : public recv_buffer_sequence_view {
public:
  recv_buffer_sequence() = default;

  recv_buffer_sequence( recv_buffer_sequence const& ) = delete;
  recv_buffer_sequence& operator=( recv_buffer_sequence const& ) = delete;

  recv_buffer_sequence( recv_buffer_sequence&& rhs ) noexcept = delete;
  recv_buffer_sequence& operator=( recv_buffer_sequence&& ) = delete;

  ~recv_buffer_sequence() {
    if ( !head_ ) {
      return;
    }

    auto p = head_;

    while ( p ) {
      auto old = p;
      p = recv_buffer_view( p ).header().next_;
      recv_buffer::aligned_delete( old );
    }
  }

  void push_back( recv_buffer rbuf ) {
    auto p = rbuf.to_raw_parts();

    if ( tail_ ) {
      auto* ptail_header = reinterpret_cast<detail::buf_header_impl*>( tail_ );
      ptail_header->next_ = p;
    }

    tail_ = p;
    if ( head_ == nullptr ) {
      head_ = tail_;
    }
  }
};

namespace detail {

struct buf_ring {
private:
  std::vector<recv_buffer> bufs_;
  io_uring* ring_ = nullptr;
  io_uring_buf_ring* buf_ring_ = nullptr;
  std::uint16_t bgid_ = 0;

public:
  buf_ring() = delete;

  buf_ring( buf_ring const& ) = delete;
  buf_ring& operator=( buf_ring const& ) = delete;

  buf_ring( buf_ring&& rhs ) = delete;
  buf_ring& operator=( buf_ring&& rhs ) = delete;

  FIONA_DECL
  buf_ring( io_uring* ring, std::uint32_t num_bufs, std::uint16_t bgid );

  FIONA_DECL
  buf_ring( io_uring* ring, std::uint32_t num_bufs, std::size_t buf_size, std::uint16_t bgid );

  // TODO: this probably needs to be exported but we currently lack test coverage for this
  ~buf_ring();

  recv_buffer& get_buf( std::size_t idx ) noexcept { return bufs_[idx]; }
  io_uring_buf_ring* get() const noexcept { return buf_ring_; }
  std::uint32_t size() const noexcept { return static_cast<std::uint32_t>( bufs_.size() ); }
  std::uint16_t bgid() const noexcept { return bgid_; }
};

struct hasher {
  using is_transparent = void;

  template <class Promise>
  std::size_t operator()( std::coroutine_handle<Promise> h ) const noexcept {
    return ( *this )( h.address() );
  }

  std::size_t operator()( void* p ) const noexcept {
    boost::hash<void*> hasher;
    return hasher( p );
  }
};

struct key_equal {
  using is_transparent = void;

  template <class Promise1, class Promise2>
  bool operator()( std::coroutine_handle<Promise1> const h1, std::coroutine_handle<Promise2> const h2 ) const noexcept {
    return h1.address() == h2.address();
  }

  template <class Promise>
  bool operator()( std::coroutine_handle<Promise> const h, void* p ) const noexcept {
    return h.address() == p;
  }

  template <class Promise>
  bool operator()( void* p, std::coroutine_handle<Promise> const h ) const noexcept {
    return h.address() == p;
  }
};

using task_map_type = boost::unordered_flat_map<std::coroutine_handle<>, int*, hasher, key_equal>;

struct io_context_frame {
  io_uring io_ring_;
  std::mutex m_;
  task_map_type tasks_;
  io_context_params params_;
  std::list<buf_ring> buf_rings_;
  boost::unordered_flat_set<int> fds_;
  std::deque<std::coroutine_handle<>> run_queue_;
  std::exception_ptr exception_ptr_;
  std::array<int, 2> pipefd_ = { -1, -1 };

  FIONA_DECL
  io_context_frame( io_context_params const& io_ctx_params );

  FIONA_DECL
  ~io_context_frame();
};

} // namespace detail
} // namespace fiona

#endif // FIONA_DETAIL_COMMON_HPP
