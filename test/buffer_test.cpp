#include "helpers.hpp"

#include <fiona/buffer.hpp>

#include <algorithm>
#include <iterator>
#include <vector>

TEST_CASE( "recv_buffer" )
{
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

TEST_CASE( "owned buffer sequence" )
{
  fiona::recv_buffer_sequence buf_seq;
  CHECK( buf_seq.num_bufs() == 0 );
  CHECK( buf_seq.empty() );

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

  CHECK( buf_seq.num_bufs() == strs.size() );

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

TEST_CASE( "end() stability" )
{
  fiona::recv_buffer_sequence buf_seq;
  auto end = buf_seq.end();

  buf_seq.push_back( fiona::recv_buffer( 128 ) );
  CHECK( end == buf_seq.end() );
  CHECK( ( *--end ).capacity() == 128 );
  CHECK( end == buf_seq.begin() );
}

TEST_CASE( "move stability" )
{
  fiona::recv_buffer_sequence buf_seq;
  for ( int i = 0; i < 16; ++i ) {
    buf_seq.push_back( fiona::recv_buffer( 128 ) );
  }

  CHECK( std::ranges::distance( buf_seq ) == 16 );
  std::ranges::for_each( buf_seq, []( fiona::recv_buffer_view buf ) {
    CHECK( buf.capacity() == 128 );
  } );

  std::vector<unsigned char*> old_addrs( 16 );
  std::ranges::transform(
      buf_seq, old_addrs.begin(),
      []( fiona::recv_buffer_view b ) { return b.data(); } );

  auto pos = buf_seq.begin();

  fiona::recv_buffer_sequence buf_seq2( std::move( buf_seq ) );
  while ( pos != buf_seq2.end() ) {
    CHECK( ( *pos ).capacity() == 128 );
    ++pos;
  }

  CHECK( std::ranges::equal(
      old_addrs, buf_seq2, {}, {},
      []( fiona::recv_buffer_view b ) { return b.data(); } ) );

  CHECK( buf_seq.end() != buf_seq2.end() );

  for ( int i = 0; i < 8; ++i ) {
    buf_seq.push_back( fiona::recv_buffer( 256 ) );
  }

  CHECK( std::ranges::distance( buf_seq ) == 8 );
  std::ranges::for_each( buf_seq, []( fiona::recv_buffer_view buf ) {
    CHECK( buf.capacity() == 256 );
  } );
}

TEST_CASE( "push_back empty" )
{
  fiona::recv_buffer_sequence bs;
  bs.push_back( fiona::recv_buffer( 0 ) );
  CHECK( bs.num_bufs() == 1 );
}

TEST_CASE( "concat" )
{
  {
    fiona::recv_buffer_sequence bs1, bs2;
    bs1.concat( std::move( bs2 ) );
    CHECK( bs1.empty() );
    CHECK( bs2.empty() );
  }

  {
    fiona::recv_buffer_sequence bs1, bs2;
    bs1.push_back( fiona::recv_buffer( 1 ) );
    bs1.push_back( fiona::recv_buffer( 2 ) );

    bs1.concat( std::move( bs2 ) );
    {
      CHECK( bs1.num_bufs() == 2 );
      CHECK( bs2.num_bufs() == 0 );

      unsigned cap = 0;
      for ( auto bv : bs1 ) {
        CHECK( bv.capacity() == ++cap );
      }
    }
  }

  {
    fiona::recv_buffer_sequence bs1, bs2;
    bs2.push_back( fiona::recv_buffer( 1 ) );
    bs2.push_back( fiona::recv_buffer( 2 ) );

    bs1.concat( std::move( bs2 ) );
    {
      CHECK( bs1.num_bufs() == 2 );
      CHECK( bs2.num_bufs() == 0 );

      unsigned cap = 0;
      for ( auto bv : bs1 ) {
        CHECK( bv.capacity() == ++cap );
      }
    }
  }

  {
    fiona::recv_buffer_sequence bs1, bs2;
    bs1.push_back( fiona::recv_buffer( 1 ) );
    bs1.push_back( fiona::recv_buffer( 2 ) );
    bs2.push_back( fiona::recv_buffer( 3 ) );
    bs2.push_back( fiona::recv_buffer( 4 ) );

    auto p1 = bs1.begin();
    auto p2 = bs2.begin();

    auto end1 = bs1.end();

    bs1.concat( std::move( bs2 ) );

    {
      CHECK( bs1.num_bufs() == 4 );
      CHECK( bs2.num_bufs() == 0 );

      unsigned cap = 0;
      for ( auto bv : bs1 ) {
        CHECK( bv.capacity() == ++cap );
      }
    }

    {
      CHECK( ( *--end1 ).capacity() == 4 );

      CHECK( ( *p2 ).capacity() == 3 );
      --p2;
      --p2;
      CHECK( p1 == p2 );

      CHECK( ( *p1 ).capacity() == 1 );
      ++p1;
      CHECK( ( *++p1 ).capacity() == 3 );
      CHECK( ( *++p1 ).capacity() == 4 );
      CHECK( ++p1 == bs1.end() );
    }
  }
}

TEST_CASE( "pop_front" )
{
  auto string_to_buf = []( std::string_view str ) {
    fiona::recv_buffer buf( str.size() );
    auto out = buf.spare_capacity_mut().begin();
    auto r = std::ranges::copy( str, out );
    buf.set_len( static_cast<std::size_t>( r.out - out ) );
    return buf;
  };

  auto verify_range = []( fiona::recv_buffer_sequence& buf_seq ) {
    for ( auto view : buf_seq ) {
      CHECK( view.size() > 0 );
    }
  };

  fiona::recv_buffer_sequence bufs;
  bufs.push_back( string_to_buf( "rocky" ) );
  bufs.push_back( string_to_buf( "chase" ) );
  bufs.push_back( string_to_buf( "rubble" ) );
  bufs.push_back( string_to_buf( "zouma" ) );
  bufs.push_back( string_to_buf( "skye" ) );

  CHECK( bufs.num_bufs() == 5 );

  CHECK( bufs.pop_front().as_str() == "rocky" );
  CHECK( ( *bufs.begin() ).as_str() == "chase" );
  verify_range( bufs );
  CHECK( bufs.num_bufs() == 4 );

  CHECK( bufs.pop_front().as_str() == "chase" );
  CHECK( ( *bufs.begin() ).as_str() == "rubble" );
  verify_range( bufs );
  CHECK( bufs.num_bufs() == 3 );

  CHECK( bufs.pop_front().as_str() == "rubble" );
  CHECK( ( *bufs.begin() ).as_str() == "zouma" );
  verify_range( bufs );
  CHECK( bufs.num_bufs() == 2 );

  CHECK( bufs.pop_front().as_str() == "zouma" );
  CHECK( ( *bufs.begin() ).as_str() == "skye" );
  verify_range( bufs );
  CHECK( bufs.num_bufs() == 1 );

  CHECK( bufs.pop_front().as_str() == "skye" );
  CHECK( bufs.begin() == bufs.end() );
  verify_range( bufs );
  CHECK( bufs.num_bufs() == 0 );
}
