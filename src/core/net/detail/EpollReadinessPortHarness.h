#pragma once

// Source-private deterministic decoder seam for repository contracts only.

#ifndef _WIN32

#include "EpollReadinessPort.h"

#include <span>

namespace gamenet::net::detail {

struct NativeReadinessEvent {
    std::uint64_t generation{0};
    std::uint32_t events{0};
};

class EpollReadinessPortHarness final {
public:
    EpollReadinessPortHarness() = delete;

    static ReadinessWaitResult decode(
        EpollReadinessPort& port,
        std::span<const NativeReadinessEvent> nativeEvents) {
        port.assertOwnerThread();
        port.resetNoticeBatch();
        for (const auto& nativeEvent : nativeEvents) {
            ++port.progress_.nativeNotices;
            (void)port.appendNativeNotice(
                nativeEvent.generation,
                nativeEvent.events);
        }
        port.progress_.deliveredNotices = port.notices_.size();
        return port.currentResult(gamenet::base::now());
    }
};

}  // namespace gamenet::net::detail

#endif
