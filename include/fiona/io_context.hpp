#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

// clang-format off
#include <fiona/params.hpp>         // for io_context_params
#include <fiona/task.hpp>           // for task

#include <fiona/detail/common.hpp>  // for buf_ring, io_context_frame

#include <cstdint>                  // for uint16_t
#include <cstring>                  // for size_t
#include <memory>                   // for shared_ptr, __shared_ptr_access, make_shared
#include <utility>                  // for move
#include <vector>                   // for vector

namespace fiona { struct executor; }
// clang-format on

namespace fiona {

struct io_context {
private:
  std::shared_ptr<detail::io_context_frame> pframe_;

public:
  io_context( io_context_params const& params = {} )
      : pframe_( std::make_shared<detail::io_context_frame>( params ) ) {}

  ~io_context();

  executor get_executor() const noexcept;
  io_context_params params() const noexcept { return pframe_->params_; }

  void post( task<void> t );

  void register_buffer_sequence( std::size_t num_bufs, std::size_t buf_size,
                                 std::uint16_t buffer_group_id ) {
    auto ring = &pframe_->io_ring_;
    pframe_->buf_rings_.emplace_back( ring, num_bufs, buf_size,
                                      buffer_group_id );
  }

  void run();
};

} // namespace fiona

#endif // FIONA_IO_CONTEXT_HPP
