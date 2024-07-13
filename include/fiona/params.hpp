// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef FIONA_PARAMS_HPP
#define FIONA_PARAMS_HPP

#include <cstdint>

namespace fiona {
struct io_context_params
{
  std::uint32_t sq_entries = 4096;
  std::uint32_t cq_entries = 4096;
  std::uint32_t num_files = 1024;
};
} // namespace fiona

#endif // FIONA_PARAMS_HPP
