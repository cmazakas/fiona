#include <fiona/buffer.hpp>

#include <boost/buffers/mutable_buffer.hpp>
#include <boost/buffers/tag_invoke.hpp>

#include <boost/buffers/buffer.hpp>
#include <boost/buffers/buffer_copy.hpp>
#include <boost/buffers/buffer_size.hpp>
#include <boost/buffers/mutable_buffer.hpp>

#include <string>

#include "buffers_adaptor.hpp"

namespace fiona {

std::string
recv_buffer_sequence_view::to_string() const {
  detail::buffers_adaptor from{ *this };

  auto n = boost::buffers::buffer_size( from );

  std::string s( n, '\0' );

  boost::buffers::buffer_copy( boost::buffers::buffer( s.data(), s.size() ),
                               from );

  return s;
}

std::vector<unsigned char>
recv_buffer_sequence_view::to_bytes() const {
  detail::buffers_adaptor from{ *this };

  auto n = boost::buffers::buffer_size( from );

  std::vector<unsigned char> s( n, 0x00 );

  boost::buffers::buffer_copy( boost::buffers::buffer( s.data(), s.size() ),
                               from );

  return s;
}

} // namespace fiona
