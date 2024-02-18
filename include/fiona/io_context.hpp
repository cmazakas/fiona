#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

#include <fiona/detail/common.hpp> // for io_context_frame, buf_ring
#include <fiona/detail/config.hpp> // for FIONA_DECL
#include <fiona/params.hpp>        // for io_context_params
#include <fiona/task.hpp>          // for task

#include <cstdint>                 // for uint16_t
#include <cstring>                 // for size_t
#include <list>                    // for list
#include <memory>                  // for shared_ptr, __shared_ptr_access, make_shared

namespace fiona {
struct executor;
} // namespace fiona

namespace fiona {

struct io_context {
private:
  std::shared_ptr<detail::io_context_frame> pframe_;

public:
  io_context( io_context_params const& params = {} )
      : pframe_( std::make_shared<detail::io_context_frame>( params ) ) {}

  FIONA_DECL
  ~io_context();

  FIONA_DECL
  executor get_executor() const noexcept;

  io_context_params params() const noexcept { return pframe_->params_; }

  FIONA_DECL
  void spawn( task<void> t );

  void register_buffer_sequence( std::size_t num_bufs, std::size_t buf_size, std::uint16_t buffer_group_id ) {
    auto ring = &pframe_->io_ring_;
    pframe_->buf_rings_.emplace_back( ring, num_bufs, buf_size, buffer_group_id );
  }

  FIONA_DECL
  void run();
};

} // namespace fiona

#endif // FIONA_IO_CONTEXT_HPP
