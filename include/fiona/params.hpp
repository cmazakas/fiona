#ifndef FIONA_PARAMS_HPP
#define FIONA_PARAMS_HPP

#include <cstdint>

namespace fiona {
struct io_context_params {
  std::uint32_t sq_entries = 4096;
  std::uint32_t cq_entries = 4096;
  std::uint32_t num_files = 1024;
};
} // namespace fiona

#endif // FIONA_PARAMS_HPP
