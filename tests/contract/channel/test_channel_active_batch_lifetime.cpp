#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"
#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"
#include "support/TestAssert.h"

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

namespace {

struct SocketPair {
    gamenet::net::SocketFd first{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd second{gamenet::net::kInvalidSocket};

    SocketPair() {
#ifdef _WIN32
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        first = fds[0];
        second = fds[1];
#else
        int fds[2];
        GAMENET_TEST_ASSERT(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        first = fds[0];
        second = fds[1];
#endif
    }

    ~SocketPair() {
        gamenet::net::sockets::close(first);
        gamenet::net::sockets::close(second);
    }
};

struct ChannelOwner {
    ChannelOwner(
        gamenet::net::EventLoop* loop,
        gamenet::net::SocketFd fd,
        bool* destroyed)
        : channel(loop, fd), destroyed(destroyed) {}

    ~ChannelOwner() {
        *destroyed = true;
    }

    gamenet::net::Channel channel;
    bool* destroyed;
};

using ActiveBatchHarness =
    gamenet::net::detail::EventLoopActiveBatchHarness;

void testPendingPeerRemovalInvalidatesItsCapturedSlot() {
    gamenet::net::EventLoop loop;
    std::array<SocketPair, 2> sockets;
    std::array<bool, 2> destroyed{false, false};
    std::array<int, 2> callbacks{0, 0};
    std::array<std::shared_ptr<ChannelOwner>, 2> owners{
        std::make_shared<ChannelOwner>(&loop, sockets[0].first, &destroyed[0]),
        std::make_shared<ChannelOwner>(&loop, sockets[1].first, &destroyed[1]),
    };

    for (std::size_t index = 0; index < owners.size(); ++index) {
        owners[index]->channel.tie(owners[index]);
        owners[index]->channel.enableReading();
        owners[index]->channel.setRevents(gamenet::net::Channel::kReadEvent);
    }

    owners[0]->channel.setReadCallback(
        [&](gamenet::base::Timestamp) {
            ++callbacks[0];
            ChannelOwner* victim = owners[1].get();
            victim->channel.disableAll();
            victim->channel.remove();
            owners[1].reset();
            GAMENET_TEST_ASSERT(destroyed[1]);
        });
    owners[1]->channel.setReadCallback(
        [&](gamenet::base::Timestamp) { ++callbacks[1]; });

    ActiveBatchHarness::dispatch(
        loop,
        std::vector<gamenet::net::Channel*>{
            &owners[0]->channel,
            &owners[1]->channel,
        },
        gamenet::base::now());

    GAMENET_TEST_ASSERT(callbacks[0] == 1);
    GAMENET_TEST_ASSERT(callbacks[1] == 0);
    GAMENET_TEST_ASSERT(destroyed[1]);

    owners[0]->channel.disableAll();
    owners[0]->channel.remove();
    owners[0].reset();
    GAMENET_TEST_ASSERT(destroyed[0]);
}

void testCurrentTieKeepsOwnerAliveUntilCallbackReturns() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    bool destroyed = false;
    int callbacks = 0;
    auto owner =
        std::make_shared<ChannelOwner>(&loop, sockets.first, &destroyed);
    owner->channel.tie(owner);
    owner->channel.enableReading();
    owner->channel.setRevents(gamenet::net::Channel::kReadEvent);

    gamenet::net::Channel* channel = &owner->channel;
    channel->setReadCallback([&](gamenet::base::Timestamp) {
        ++callbacks;
        channel->disableAll();
        channel->remove();
        owner.reset();
        GAMENET_TEST_ASSERT(!destroyed);
    });

    ActiveBatchHarness::dispatch(
        loop,
        std::vector<gamenet::net::Channel*>{channel},
        gamenet::base::now());

    GAMENET_TEST_ASSERT(callbacks == 1);
    GAMENET_TEST_ASSERT(!owner);
    GAMENET_TEST_ASSERT(destroyed);
}

void testStaleRepeatedRemoveCannotEraseSameFdReplacement() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    gamenet::net::Channel stale(&loop, sockets.first);
    gamenet::net::Channel replacement(&loop, sockets.first);

    stale.enableReading();
    stale.disableAll();
    stale.remove();

#ifdef _WIN32
    // A Windows socket remains associated with its original IOCP after Channel
    // removal. Preserve that known association when the same live socket is
    // transferred to the replacement Channel.
    gamenet::net::detail::EventLoopIocpAssociationHarness::
        preserveSocketAssociation(loop, sockets.first);
#endif
    replacement.enableReading();
    GAMENET_TEST_ASSERT(loop.hasChannel(&replacement));

    bool rejected = false;
    try {
        stale.remove();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    GAMENET_TEST_ASSERT(rejected);
    GAMENET_TEST_ASSERT(loop.hasChannel(&replacement));

    int callbacks = 0;
    replacement.setReadCallback(
        [&](gamenet::base::Timestamp) { ++callbacks; });
    replacement.setRevents(gamenet::net::Channel::kReadEvent);
    ActiveBatchHarness::dispatch(
        loop,
        std::vector<gamenet::net::Channel*>{&replacement},
        gamenet::base::now());
    GAMENET_TEST_ASSERT(callbacks == 1);

    replacement.disableAll();
    replacement.disableAll();
    GAMENET_TEST_ASSERT(loop.hasChannel(&replacement));
    replacement.remove();
}

void testRemoveReregisterStopsRemainingOldReadinessCallbacks() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    gamenet::net::Channel channel(&loop, sockets.first);
    std::vector<int> callbacks;

    channel.setErrorCallback([&] {
        callbacks.push_back(1);
        channel.disableAll();
        channel.remove();
#ifdef _WIN32
        gamenet::net::detail::EventLoopIocpAssociationHarness::
            preserveSocketAssociation(loop, sockets.first);
#endif
        channel.enableReading();
    });
    channel.setReadCallback(
        [&](gamenet::base::Timestamp) { callbacks.push_back(2); });
    channel.setWriteCallback([&] { callbacks.push_back(3); });

    channel.enableReading();
    channel.enableWriting();
    channel.setRevents(
        gamenet::net::Channel::kErrorEvent |
        gamenet::net::Channel::kReadEvent |
        gamenet::net::Channel::kWriteEvent);

    ActiveBatchHarness::dispatch(
        loop,
        std::vector<gamenet::net::Channel*>{&channel},
        gamenet::base::now());

    GAMENET_TEST_ASSERT((callbacks == std::vector<int>{1}));
    GAMENET_TEST_ASSERT(loop.hasChannel(&channel));

    channel.disableAll();
    channel.remove();
}

void testCurrentChannelRetirementDoesNotUseSaturatedFunctorQueue() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
    });
    SocketPair sockets;
    auto channel =
        std::make_unique<gamenet::net::Channel>(&loop, sockets.first);
    channel->enableReading();
    channel->setRevents(gamenet::net::Channel::kReadEvent);

    GAMENET_TEST_ASSERT(loop.tryQueueInLoop([] {}));
    loop.queueInLoop([] {});
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);

    gamenet::net::Channel* rawChannel = channel.get();
    rawChannel->setReadCallback([&](gamenet::base::Timestamp) {
        auto removed = std::move(channel);
        removed->disableAll();
        removed->remove();
        ActiveBatchHarness::retireCurrentChannel(
            loop,
            std::move(removed));
        GAMENET_TEST_ASSERT(!channel);
        GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);
        GAMENET_TEST_ASSERT(
            ActiveBatchHarness::hasRetiredCurrentChannel(loop));
    });

    ActiveBatchHarness::dispatch(
        loop,
        std::vector<gamenet::net::Channel*>{rawChannel},
        gamenet::base::now());

    GAMENET_TEST_ASSERT(!channel);
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);
    GAMENET_TEST_ASSERT(
        !ActiveBatchHarness::hasRetiredCurrentChannel(loop));
}

}  // namespace

int main() {
    testPendingPeerRemovalInvalidatesItsCapturedSlot();
    testCurrentTieKeepsOwnerAliveUntilCallbackReturns();
    testStaleRepeatedRemoveCannotEraseSameFdReplacement();
    testRemoveReregisterStopsRemainingOldReadinessCallbacks();
    testCurrentChannelRetirementDoesNotUseSaturatedFunctorQueue();
    return 0;
}
