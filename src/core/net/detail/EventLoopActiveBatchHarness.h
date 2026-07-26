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
        loop.assertInLoopThread();
        if (loop.looping_ || loop.eventHandling_) {
            throw std::logic_error(
                "active-batch harness requires an idle owner EventLoop");
        }
        loop.activeChannels_ = std::move(channels);
        loop.pollReturnTime_ = receiveTime;
        loop.dispatchActiveChannels();
        loop.activeChannels_.clear();
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
