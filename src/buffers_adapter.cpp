#include "buffers_adapter.hpp"

namespace fiona {
namespace detail {
recv_buffer_sequence_view
tag_invoke( boost::buffers::prefix_tag, buffers_adapter const& seq,
            std::size_t n ) {

  std::size_t total_len = 0;
  auto seq_view = seq.buf_seq_view_;
  auto end = seq_view.end();

  for ( auto pos = seq_view.begin(); pos != end; ++pos ) {
    auto buf = *pos;
    auto new_len = total_len + buf.size();
    if ( new_len > n ) {
      buf.set_len( buf.size() - ( new_len - n ) );
    }

    if ( total_len == n ) {
      buf.set_len( 0 );
      continue;
    }

    total_len += buf.size();
  }

  return seq_view;
}
} // namespace detail
} // namespace fiona
