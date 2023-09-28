#include <fiona/tcp.hpp>

namespace fiona {
namespace tcp {

stream::~
stream() {
  if ( fd_ >= 0 ) {
    auto ring = detail::executor_access_policy::ring( ex_ );
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_close_direct( sqe, fd_ );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_submit( ring );

    detail::executor_access_policy::release_fd( ex_, fd_ );
  }
}

stream&
stream::operator=( stream&& rhs ) noexcept {
  if ( this != &rhs ) {
    if ( fd_ >= 0 ) {
      auto ring = detail::executor_access_policy::ring( ex_ );
      auto sqe = fiona::detail::get_sqe( ring );
      io_uring_prep_close_direct( sqe, fd_ );
      io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
      io_uring_sqe_set_data( sqe, nullptr );
      io_uring_submit( ring );

      detail::executor_access_policy::release_fd( ex_, fd_ );
    }

    ex_ = std::move( rhs.ex_ );
    fd_ = rhs.fd_;
    ts_ = rhs.ts_;

    rhs.fd_ = -1;
    rhs.ts_ = {};
  }
  return *this;
}

void
stream::recv_awaitable::frame::await_process_cqe( io_uring_cqe* cqe ) {
  if ( ( cqe->res >= 0 ) && ( cqe->flags & IORING_CQE_F_BUFFER ) ) {
    std::uint16_t bid = cqe->flags >> 16;
    auto buf = br_.get_buffer( bid );
    buffers_.push_back( borrowed_buffer( br_.get(), buf.data(), buf.size(),
                                         br_.size(), bid, cqe->res ) );
  } else {
    buffers_.push_back( fiona::error_code::from_errno( -cqe->res ) );
  }

  if ( !( cqe->flags & IORING_CQE_F_MORE ) ) {
    initiated_ = false;
  }

  if ( ( cqe->flags & IORING_CQE_F_MORE ) ) {
    intrusive_ptr_add_ref( this );
  }
}

stream::recv_awaitable::~
recv_awaitable() {
  auto& self = *p_;
  if ( self.initiated_ ) {
    auto ring = detail::executor_access_policy::ring( self.ex_ );
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, p_.get(), 0 );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_submit( ring );
  }
}

void
stream::recv_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& self = *p_;
  self.h_ = h;
  if ( self.initiated_ ) {
    return;
  }

  auto ring = detail::executor_access_policy::ring( self.ex_ );
  auto sqe = fiona::detail::get_sqe( ring );
  io_uring_prep_recv_multishot( sqe, self.fd_, nullptr, 0, 0 );
  io_uring_sqe_set_flags( sqe, IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE );
  io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
  sqe->buf_group = self.bgid_;

  self.initiated_ = true;
}

result<borrowed_buffer>
stream::recv_awaitable::await_resume() {
  auto& self = *p_;
  BOOST_ASSERT( !self.buffers_.empty() );
  auto borrowed_buf = std::move( self.buffers_.front() );
  self.buffers_.pop_front();

  return borrowed_buf;
}

stream::write_awaitable::~
write_awaitable() {
  auto& self = *p_;
  if ( self.initiated_ && !self.done_ ) {
    auto ring = self.ring_;
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_cancel( sqe, p_.get(), 0 );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_submit( ring );
  }
}

void
stream::write_awaitable::await_suspend( std::coroutine_handle<> h ) {
  auto& self = *p_;
  if ( self.initiated_ ) {
    return;
  }

  auto ring = self.ring_;

  if ( io_uring_sq_space_left( ring ) < 2 ) {
    io_uring_submit( ring );
  }

  {
    auto sqe = fiona::detail::get_sqe( ring );
    io_uring_prep_send( sqe, self.fd_, self.buf_, self.nbytes_, MSG_WAITALL );
    io_uring_sqe_set_data( sqe, boost::intrusive_ptr( p_ ).detach() );
    io_uring_sqe_set_flags( sqe, IOSQE_IO_LINK | IOSQE_FIXED_FILE );
  }

  {
    auto sqe = fiona::detail::get_sqe( ring );

    io_uring_prep_link_timeout( sqe, &self.ts_, 0 );
    io_uring_sqe_set_data( sqe, nullptr );
    io_uring_sqe_set_flags( sqe, IOSQE_CQE_SKIP_SUCCESS );
  }

  self.h_ = h;
  self.initiated_ = true;
}

} // namespace tcp
} // namespace fiona
