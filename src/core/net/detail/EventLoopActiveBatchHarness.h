#pragma once

// Source-private deterministic contract harness for EventLoop active batches.
// It is compiled only by repository tests and is not part of installed API.

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::net::detail {

class EventLoopActiveBatchHarness final {
public:
    EventLoopActiveBatchHarness() = delete;

    static void dispatch(
        EventLoop& loop,
        std::vector<Channel*> channels,
        gamenet::base::Timestamp receiveTime) {
        install(loop, std::move(channels), receiveTime);
        while (loop.hasPendingActiveChannels()) {
            loop.dispatchActiveChannels();
        }
    }

    static void install(
        EventLoop& loop,
        std::vector<Channel*> channels,
        gamenet::base::Timestamp receiveTime) {
        loop.assertInLoopThread();
        if (loop.looping_ || loop.eventHandling_) {
            throw std::logic_error(
                "active-batch harness requires an idle owner EventLoop");
        }
        if (loop.hasPendingActiveChannels()) {
            throw std::logic_error(
                "active-batch harness cannot replace pending ready work");
        }
        loop.activeChannels_ = std::move(channels);
        loop.activeChannelCursor_ = 0;
        loop.pollReturnTime_ = receiveTime;
    }

    static void dispatchRound(EventLoop& loop) {
        loop.assertInLoopThread();
        loop.dispatchActiveChannels();
    }

    static std::size_t pendingCount(const EventLoop& loop) noexcept {
        std::size_t pending = 0;
        for (std::size_t index = loop.activeChannelCursor_;
             index < loop.activeChannels_.size();
             ++index) {
            if (loop.activeChannels_[index] != nullptr) {
                ++pending;
            }
        }
        return pending;
    }

    static void runFairRound(
        EventLoop& loop,
        std::vector<Channel*> channels,
        gamenet::base::Timestamp receiveTime) {
        install(loop, std::move(channels), receiveTime);
        loop.dispatchActiveChannels();
        loop.doExpiredTimers(gamenet::base::now());
        loop.doControlSources();
        loop.doLifecycleNodes();
        loop.doPendingFunctors(
            loop.options_.maxFunctorsPerIteration);
    }

    static void configurePendingFunctorCapacity(
        EventLoop& loop,
        std::size_t normalCapacity,
        std::size_t reserveCapacity,
        std::size_t perIterationCapacity) {
        loop.assertInLoopThread();
        if (loop.looping_ ||
            loop.callingPendingFunctors_.load(std::memory_order_relaxed) ||
            !loop.pendingFunctors_.empty()) {
            throw std::logic_error(
                "pending-functor capacity harness requires an idle EventLoop");
        }

        EventLoopOptions options = loop.options_;
        options.maxPendingFunctors = normalCapacity;
        options.reservedPendingFunctors = reserveCapacity;
        options.maxFunctorsPerIteration = perIterationCapacity;
        options.validate();
        loop.options_ = options;
    }

    static void retireCurrentChannel(
        EventLoop& loop,
        std::unique_ptr<Channel> channel) noexcept {
        loop.retireCurrentChannel(std::move(channel));
    }

    static bool hasRetiredCurrentChannel(const EventLoop& loop) noexcept {
        return static_cast<bool>(loop.retiredCurrentChannel_);
    }
};

}  // namespace gamenet::net::detail
