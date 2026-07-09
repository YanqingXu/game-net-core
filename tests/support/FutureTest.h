#pragma once

#include "support/TestAssert.h"

#include <chrono>
#include <future>

namespace gamenet::test {

template <typename Future, typename Rep, typename Period>
void waitUntilReady(Future& future,
                    std::chrono::duration<Rep, Period> timeout,
                    const char* message) {
    if (future.wait_for(timeout) != std::future_status::ready) {
        GAMENET_TEST_FAIL(message);
    }
}

}  // namespace gamenet::test
