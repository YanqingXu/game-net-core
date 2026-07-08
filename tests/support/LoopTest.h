#pragma once

#include "gamenet/core/net/EventLoop.h"

#include "support/TestAssert.h"

#include <chrono>

namespace gamenet::test {

template <typename Rep, typename Period>
void runLoopWithTimeout(
    gamenet::net::EventLoop& loop,
    std::chrono::duration<Rep, Period> timeout,
    const char* message) {
    loop.runAfter(timeout, [message] {
        GAMENET_TEST_FAIL(message);
    });
    loop.loop();
}

}  // namespace gamenet::test
