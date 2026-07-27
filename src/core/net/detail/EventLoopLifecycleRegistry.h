#pragma once

// Source-private access boundary for EventLoop's dynamic lifecycle hub.
// Runtime participants may receive a signal capability, but installed callers
// cannot attach arbitrary callbacks to the internal lifecycle lane.

#include "gamenet/core/net/EventLoop.h"

#include <utility>

namespace gamenet::net::detail {

class EventLoopLifecycleRegistry final {
public:
    EventLoopLifecycleRegistry() = delete;

    static EventLoopLifecycleSource attach(
        EventLoop& loop,
        EventLoop::Functor callback) {
        return loop.attachLifecycleNode(std::move(callback));
    }

    static void detach(
        EventLoop& loop,
        const EventLoopLifecycleSource& source) {
        loop.detachLifecycleNode(source);
    }
};

}  // namespace gamenet::net::detail
