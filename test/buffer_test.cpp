#include "helpers.hpp"

#include <fiona/buffer.hpp>

#include <algorithm>
#include <iterator>
#include <vector>

TEST_CASE( "buffer_test - recv_buffer" ) {
  {
    fiona::recv_buffer buf;
    CHECK( buf.data() != nullptr );
    CHECK( buf.size() == 0 );
    CHECK( buf.capacity() == 0 );
    CHECK( buf.empty() );
    CHECK( buf.as_str() == "" );
    CHECK( buf.readable_bytes().empty() );
    CHECK( buf.spare_capacity_mut().empty() );
  }

  {
    fiona::recv_buffer buf( 1024 );
    CHECK( buf.data() != nullptr );
    CHECK( buf.size() == 0 );
    CHECK( buf.capacity() == 1024 );
    CHECK( buf.empty() );
    CHECK( buf.as_str() == "" );
    CHECK( buf.readable_bytes().empty() );
    CHECK( buf.spare_capacity_mut().size() == buf.capacity() );
  }

  {
    fiona::recv_buffer buf0( 1024 );
    for ( auto& b : buf0.spare_capacity_mut() ) {
      b = 0x12;
    }
    buf0.set_len( buf0.capacity() );

    fiona::recv_buffer buf( std::move( buf0 ) );

    CHECK( buf0.data() != nullptr );
    CHECK( buf0.size() == 0 );
    CHECK( buf0.capacity() == 0 );
    CHECK( buf0.empty() );
    CHECK( buf0.as_str() == "" );
    CHECK( buf0.readable_bytes().empty() );
    CHECK( buf0.spare_capacity_mut().empty() );

    CHECK( buf.data() != nullptr );
    CHECK( buf.size() == buf.capacity() );
    CHECK( buf.capacity() == 1024 );
    CHECK( !buf.empty() );
    CHECK( buf.as_str() != "" );
    CHECK( buf.readable_bytes().size() == buf.capacity() );
    CHECK( buf.spare_capacity_mut().empty() );
  }

  {
    fiona::recv_buffer buf0( 1024 );
    fiona::recv_buffer buf;

    buf = std::move( buf0 );

    CHECK( buf0.data() != nullptr );
    CHECK( buf0.size() == 0 );
    CHECK( buf0.capacity() == 0 );
    CHECK( buf0.empty() );
    CHECK( buf0.as_str() == "" );
    CHECK( buf0.readable_bytes().empty() );
    CHECK( buf0.spare_capacity_mut().empty() );

    CHECK( buf.data() != nullptr );
    CHECK( buf.size() == 0 );
    CHECK( buf.capacity() == 1024 );
    CHECK( buf.empty() );
    CHECK( buf.as_str() == "" );
    CHECK( buf.readable_bytes().empty() );
    CHECK( buf.spare_capacity_mut().size() == 1024 );
  }
}

TEST_CASE( "buffer_test - owned buffer sequence" ) {
  fiona::recv_buffer_sequence buf_seq;

  auto register_buffer = [&buf_seq]( std::string_view str ) {
    fiona::recv_buffer buf( 1024 );
    std::ranges::copy( str, buf.spare_capacity_mut().begin() );
    buf.set_len( str.size() );
    CHECK( buf.as_str() == str );
    buf_seq.push_back( std::move( buf ) );
  };

  std::string_view str1 = "hello, world!";
  std::string_view str2 = "I love C++!";
  std::string_view str3 = "io_uring is pretty great too.";
  std::string_view str4 = "Asio was the inspiration for Fiona. It was a true "
                          "pioneer of its time and Fiona can only "
                          "hope to hold a candle to its immense success.";

  std::array<std::string_view, 4> strs = { str1, str2, str3, str4 };

  for ( auto str : strs ) {
    register_buffer( str );
  }

  auto pos = buf_seq.begin();

  fiona::recv_buffer_view buf_view = *pos;

  {
    CHECK( buf_view.as_str() == str1 );

    ++pos;
    CHECK( ( *pos ).as_str() == str2 );

    ++pos;
    CHECK( ( *pos ).as_str() == str3 );

    ++pos;
    CHECK( ( *pos ).as_str() == str4 );

    ++pos;
  }

  REQUIRE( pos == buf_seq.end() );

  {
    --pos;
    CHECK( ( *pos ).as_str() == str4 );

    --pos;
    CHECK( ( *pos ).as_str() == str3 );

    --pos;
    CHECK( ( *pos ).as_str() == str2 );

    --pos;
    CHECK( buf_view.as_str() == str1 );
  }

  CHECK( pos == buf_seq.begin() );
}

TEST_CASE( "buffer_test - end() stability" ) {
  fiona::recv_buffer_sequence buf_seq;
  auto end = buf_seq.end();

  buf_seq.push_back( fiona::recv_buffer( 128 ) );
  CHECK( end == buf_seq.end() );
  CHECK( ( *--end ).capacity() == 128 );
  CHECK( end == buf_seq.begin() );
}

TEST_CASE( "buffer_test - move stability" ) {
  fiona::recv_buffer_sequence buf_seq;
  for ( int i = 0; i < 16; ++i ) {
    buf_seq.push_back( fiona::recv_buffer( 128 ) );
  }

  CHECK( std::ranges::distance( buf_seq ) == 16 );
  std::ranges::for_each( buf_seq, []( fiona::recv_buffer_view buf ) {
    CHECK( buf.capacity() == 128 );
  } );

  std::vector<unsigned char*> old_addrs( 16 );
  std::ranges::transform( buf_seq, old_addrs.begin(),
                          []( fiona::recv_buffer_view b ) {
    return b.data();
  } );

  auto pos = buf_seq.begin();

  fiona::recv_buffer_sequence buf_seq2( std::move( buf_seq ) );
  while ( pos != buf_seq2.end() ) {
    CHECK( ( *pos ).capacity() == 128 );
    ++pos;
  }

  CHECK( std::ranges::equal( old_addrs, buf_seq2, {}, {},
                             []( fiona::recv_buffer_view b ) {
    return b.data();
  } ) );

  CHECK( buf_seq.end() != buf_seq2.end() );

  for ( int i = 0; i < 8; ++i ) {
    buf_seq.push_back( fiona::recv_buffer( 256 ) );
  }

  CHECK( std::ranges::distance( buf_seq ) == 8 );
  std::ranges::for_each( buf_seq, []( fiona::recv_buffer_view buf ) {
    CHECK( buf.capacity() == 256 );
  } );
}
