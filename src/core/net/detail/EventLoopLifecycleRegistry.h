// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source-private access boundary for EventLoop's dynamic lifecycle hub.
// Runtime participants may receive a signal capability, but installed callers
// cannot attach arbitrary callbacks to the internal lifecycle lane.

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Channel.h"

#include <utility>

namespace gamenet::net::detail {

using EventLoopPhaseObserverForTesting =
    void (*)(EventLoop*, EventLoopPhase) noexcept;

void setEventLoopPhaseObserverForTesting(
    EventLoopPhaseObserverForTesting observer) noexcept;

class EventLoopLifecycleRegistry final {
public:
    EventLoopLifecycleRegistry() = delete;

    static EventLoopLifecycleSource attach(
        EventLoop& loop,
        EventLoop::Functor callback) {
        return loop.attachLifecycleNode(std::move(callback));
    }

    // Attaches one source-private participant whose callback is committed by
    // the first Running -> Quiescing transition even without an external
    // signal. The participant consumes normal lifecycle-node capacity and
    // follows the same generation, self-signal, and detach rules.
    static EventLoopLifecycleSource attachQuiesceParticipant(
        EventLoop& loop,
        EventLoop::Functor callback);

    static void detach(
        EventLoop& loop,
        const EventLoopLifecycleSource& source) {
        loop.detachLifecycleNode(source);
    }

    // Non-installed owner-adapter access; does not expose a public selector.
    // Synchronous cleanup in any owner phase, including quit/final drain. This
    // neither schedules work nor changes public tryCancel admission semantics.
    static void retireTimer(EventLoop& loop, TimerId& timer);
    // TimerQueue consumes this one-use owner permission before destroying any
    // callback capture, so reentrant public cancellation cannot inherit it.
    static bool consumeTimerRetirement(const EventLoop& loop) noexcept;

    static Poller& backend(EventLoop& loop) noexcept { return *loop.poller_; }
    static void retireRemovedChannel(
        EventLoop& loop,
        std::unique_ptr<Channel> channel) noexcept {
        loop.retireCurrentChannel(std::move(channel));
    }

    static std::size_t normalPendingFunctorCapacity(
        const EventLoop& loop) noexcept {
        return loop.options_.maxPendingFunctors;
    }

    static void setPhaseObserverForTesting(
        EventLoopPhaseObserverForTesting observer) noexcept {
        setEventLoopPhaseObserverForTesting(observer);
    }
};

}  // namespace gamenet::net::detail
