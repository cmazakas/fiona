// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_IO_CONTEXT_HPP
#define FIONA_IO_CONTEXT_HPP

#include <fiona/error.hpp>                        // for throw_errno_as_err...
#include <fiona/params.hpp>                       // for io_context_params
#include <fiona/task.hpp>                         // for task

#include <fiona/detail/common.hpp>                // for io_context_frame

#include <boost/unordered/unordered_node_map.hpp> // for unordered_node_map

#include <cstdint>                                // for uint16_t
#include <cstring>                                // for size_t
#include <memory>                                 // for shared_ptr, __shar...

#include <errno.h>                                // for EEXIST

#include <fiona_export.h>                         // for FIONA_EXPORT

namespace fiona {
struct executor;
}

namespace fiona {

class io_context
{
  std::shared_ptr<detail::io_context_frame> pframe_;

public:
  io_context( io_context_params const& params = {} )
      : pframe_( std::make_shared<detail::io_context_frame>( params ) )
  {
  }

  FIONA_EXPORT
  ~io_context();

  FIONA_EXPORT
  executor get_executor() const noexcept;

  io_context_params
  params() const noexcept
  {
    return pframe_->params_;
  }

  FIONA_EXPORT
  void run();
};

} // namespace fiona

#endif // FIONA_IO_CONTEXT_HPP
