#include <fiona/buffer.hpp>

#include <fiona/detail/config.hpp>

#include <boost/buffers/mutable_buffer.hpp>
#include <boost/buffers/type_traits.hpp>

namespace fiona {
namespace detail {

struct buffers_iterator : public recv_buffer_sequence_view::iterator {
  using value_type = boost::buffers::mutable_buffer;
  using reference = boost::buffers::mutable_buffer;

  auto as_base() noexcept {
    return static_cast<recv_buffer_sequence_view::iterator*>( this );
  }
  auto as_base() const noexcept {
    return static_cast<recv_buffer_sequence_view::iterator const*>( this );
  }

  boost::buffers::mutable_buffer operator*() const noexcept {
    auto buf_view = **as_base();
    return { buf_view.data(), buf_view.size() };
  }

  buffers_iterator& operator++() {
    as_base()->operator++();
    return *this;
  }

  buffers_iterator operator++( int ) {
    auto old = *this;
    as_base()->operator++( 0 );
    return old;
  }

  buffers_iterator& operator--() {
    as_base()->operator--();
    return *this;
  }

  buffers_iterator operator--( int ) {
    auto old = *this;
    as_base()->operator--( 0 );
    return old;
  }
};

struct buffers_adapter {
  using value_type = boost::buffers::mutable_buffer;
  using const_iterator = buffers_iterator;

  recv_buffer_sequence_view buf_seq_view_;

  buffers_iterator begin() const noexcept { return { buf_seq_view_.begin() }; }
  buffers_iterator end() const noexcept { return { buf_seq_view_.end() }; }
};

static_assert( boost::buffers::detail::is_bidirectional_iterator<
               fiona::recv_buffer_sequence_view::iterator>::value );

static_assert( boost::buffers::detail::is_bidirectional_iterator<
               buffers_iterator>::value );

static_assert(
    boost::buffers::is_mutable_buffer_sequence<buffers_adapter>::value );

static_assert(
    boost::buffers::is_const_buffer_sequence<buffers_adapter>::value );

FIONA_DECL
recv_buffer_sequence_view
tag_invoke( boost::buffers::prefix_tag, buffers_adapter const& seq,
            std::size_t n );

} // namespace detail
} // namespace fiona
