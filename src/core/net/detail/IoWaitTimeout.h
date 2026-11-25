// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gamenet/core/base/Timestamp.h"
#include <chrono>
#include <limits>

namespace gamenet::net::detail {

// Compatibility waits round UP, never busy-spin for a future sub-ms timer.
inline int ceilWaitMilliseconds(std::chrono::steady_clock::duration remaining) noexcept {
    using namespace std::chrono;
    if (remaining <= remaining.zero()) return 0;
    auto rounded = duration_cast<milliseconds>(remaining);
    if (rounded < remaining) ++rounded;
    return rounded.count() >= std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max() : static_cast<int>(rounded.count());
}

}  // namespace gamenet::net::detail
