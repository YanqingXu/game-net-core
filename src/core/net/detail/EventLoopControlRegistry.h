#pragma once

// Source-private access boundary for EventLoop's bounded internal control
// lane. This header is not installed; ordinary public callers can hold and
// notify an issued capability but cannot register control work.

#include "gamenet/core/net/EventLoop.h"
#include "IoEngine.h"

#include <utility>

namespace gamenet::net::detail {

class EventLoopControlRegistry final {
public:
    EventLoopControlRegistry() = delete;

    static EventLoopControlSource registerSource(
        EventLoop& loop,
        EventLoop::Functor callback) {
        return loop.registerControlSource(std::move(callback));
    }

    static void unregisterSource(
        EventLoop& loop,
        const EventLoopControlSource& source) {
        loop.unregisterControlSource(source);
    }

    static IoEngineCapability ioEngineCapabilities(
        const EventLoop& loop) noexcept {
        return ioEngineFromPoller(*loop.poller_).capabilities();
    }

    static IoEnginePhase ioEnginePhase(const EventLoop& loop) noexcept {
        return ioEngineFromPoller(*loop.poller_).phase();
    }

    static bool ioEngineQuiescent(const EventLoop& loop) noexcept {
        return ioEngineFromPoller(*loop.poller_).quiescent();
    }
};

}  // namespace gamenet::net::detail
