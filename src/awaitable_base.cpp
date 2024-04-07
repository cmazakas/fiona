// Copyright 2024 Christian Mazakas
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "awaitable_base.hpp"

namespace fiona {
namespace detail {

ref_count::~ref_count() {}
awaitable_base::~awaitable_base() {}

} // namespace detail
} // namespace fiona
