// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef _WIN32
#include "IoWaitTimeout.h"
#include <cerrno>
#include <ctime>

namespace gamenet::net::detail {

// Factored native-call policy permits deterministic errno/deadline contracts;
// production passes inline syscall lambdas and introduces no callback domain.
template<class PreciseWait, class LegacyWait, class Now>
int waitForEpollDeadline(gamenet::base::Timestamp deadline, bool& preciseAvailable,
                         PreciseWait precise, LegacyWait legacy, Now now) {
    const auto remaining = deadline - now();
    if (remaining <= remaining.zero()) return legacy(0);
    if (preciseAvailable) {
        using namespace std::chrono;
        const auto nanos = ceil<nanoseconds>(remaining);
        const auto seconds = duration_cast<std::chrono::seconds>(nanos);
        const timespec timeout{static_cast<time_t>(seconds.count()),
                               static_cast<long>((nanos - seconds).count())};
        const int result = precise(timeout);
        if (result >= 0 || errno != ENOSYS) return result;
        preciseAvailable = false;
    }
    // Time may have passed in the failed probe. Do not restart the old delay.
    return legacy(ceilWaitMilliseconds(deadline - now()));
}
}  // namespace gamenet::net::detail
#endif
