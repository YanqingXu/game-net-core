#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "../../../src/core/net/detail/EventLoopControlRegistry.h"
#include "../../../src/core/net/detail/ReadinessPort.h"
#ifndef _WIN32
#include "../../../src/core/net/detail/EpollReadinessPort.h"
#include "../../../src/core/net/detail/EpollReadinessPortHarness.h"
#endif
#include "support/TestAssert.h"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#ifndef _WIN32
#include <sys/epoll.h>
#include <sys/socket.h>
#endif

namespace {

using EngineHarness = gamenet::net::detail::EventLoopControlRegistry;
using gamenet::net::detail::IoEngineCapability;
using gamenet::net::detail::ReadinessPortResult;
using gamenet::net::detail::hasCapability;

void testPlatformCapabilityAndInternalCapacity() {
    gamenet::net::EventLoop loop;
    const auto capabilities = EngineHarness::ioEngineCapabilities(loop);
    const auto options = EngineHarness::ioEngineOptions(loop);
    GAMENET_TEST_ASSERT(options.maxReadinessNoticesPerWait == 64);
#ifdef _WIN32
    GAMENET_TEST_ASSERT(
        !hasCapability(capabilities, IoEngineCapability::Readiness));
#else
    GAMENET_TEST_ASSERT(
        hasCapability(capabilities, IoEngineCapability::Readiness));
    GAMENET_TEST_ASSERT(
        hasCapability(capabilities, IoEngineCapability::BackendWakeup));
#endif
}

#ifndef _WIN32

using gamenet::net::detail::EpollReadinessPort;
using gamenet::net::detail::EpollReadinessPortHarness;
using gamenet::net::detail::NativeReadinessEvent;
using gamenet::net::detail::ReadinessPortOptions;
using gamenet::net::detail::ReadinessRegistrationRequest;

struct SocketPair {
    gamenet::net::SocketFd first{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd second{gamenet::net::kInvalidSocket};

    SocketPair() {
        int fds[2];
        GAMENET_TEST_ASSERT(
            ::socketpair(
                AF_UNIX,
                SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                0,
                fds) == 0);
        first = fds[0];
        second = fds[1];
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    ~SocketPair() {
        gamenet::net::sockets::close(first);
        gamenet::net::sockets::close(second);
    }
};

ReadinessRegistrationRequest request(
    gamenet::net::Channel& channel,
    std::uint32_t interests = gamenet::net::Channel::kReadEvent) {
    return {
        .source = channel.fd(),
        .target = &channel,
        .interests = interests,
    };
}

void testGenerationRejectsStaleRemoveAndMergesCurrentMask() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    gamenet::net::Channel original(&loop, sockets.first);
    gamenet::net::Channel replacement(&loop, sockets.first);
    EpollReadinessPort port(&loop, ReadinessPortOptions{
        .maxNoticesPerWait = 4,
    });
    GAMENET_TEST_ASSERT(port.options().maxNoticesPerWait == 4);
    GAMENET_TEST_ASSERT(
        port.options().triggerMode ==
        gamenet::net::detail::ReadinessTriggerMode::Level);

    bool foreignMutationRejected = false;
    std::thread foreign([&] {
        try {
            (void)port.has(&original);
        } catch (const std::runtime_error&) {
            foreignMutationRejected = true;
        }
    });
    foreign.join();
    GAMENET_TEST_ASSERT(foreignMutationRejected);
    GAMENET_TEST_ASSERT(
        port.registerOrUpdate({
            .source = sockets.first,
            .target = nullptr,
            .interests = gamenet::net::Channel::kReadEvent,
        }).result == ReadinessPortResult::RejectedInvalid);

    const auto first = port.registerOrUpdate(request(original));
    GAMENET_TEST_ASSERT(first.result == ReadinessPortResult::Accepted);
    GAMENET_TEST_ASSERT(first.identity.valid());
    const auto updated = port.registerOrUpdate(request(
        original,
        gamenet::net::Channel::kReadEvent |
            gamenet::net::Channel::kWriteEvent));
    GAMENET_TEST_ASSERT(updated.result == ReadinessPortResult::Accepted);
    GAMENET_TEST_ASSERT(updated.identity == first.identity);
    const auto disabled = port.registerOrUpdate(request(original, 0));
    GAMENET_TEST_ASSERT(disabled.result == ReadinessPortResult::Accepted);
    GAMENET_TEST_ASSERT(disabled.identity == first.identity);
    const auto reenabled = port.registerOrUpdate(request(original));
    GAMENET_TEST_ASSERT(reenabled.result == ReadinessPortResult::Accepted);
    GAMENET_TEST_ASSERT(reenabled.identity == first.identity);
    GAMENET_TEST_ASSERT(
        port.cancel(&original) == ReadinessPortResult::Accepted);

    const auto current = port.registerOrUpdate(request(
        replacement,
        gamenet::net::Channel::kReadEvent |
            gamenet::net::Channel::kWriteEvent));
    GAMENET_TEST_ASSERT(current.result == ReadinessPortResult::Accepted);
    GAMENET_TEST_ASSERT(current.identity.valid());
    GAMENET_TEST_ASSERT(current.identity.source == first.identity.source);
    GAMENET_TEST_ASSERT(
        current.identity.generation != first.identity.generation);
    GAMENET_TEST_ASSERT(
        port.cancel(&original) == ReadinessPortResult::RejectedConflict);
    GAMENET_TEST_ASSERT(port.has(&replacement));

    const std::array nativeEvents{
        NativeReadinessEvent{
            .generation = first.identity.generation,
            .events = EPOLLIN,
        },
        NativeReadinessEvent{
            .generation = current.identity.generation,
            .events = EPOLLIN,
        },
        NativeReadinessEvent{
            .generation = current.identity.generation,
            .events = EPOLLOUT,
        },
    };
    const auto decoded =
        EpollReadinessPortHarness::decode(port, nativeEvents);
    GAMENET_TEST_ASSERT(decoded.progress.staleNotices == 1);
    GAMENET_TEST_ASSERT(decoded.notices.size() == 1);
    GAMENET_TEST_ASSERT(decoded.notices[0].identity == current.identity);
    GAMENET_TEST_ASSERT(decoded.notices[0].target == &replacement);
    GAMENET_TEST_ASSERT(
        decoded.notices[0].events ==
        (gamenet::net::Channel::kReadEvent |
         gamenet::net::Channel::kWriteEvent));

    const auto readOnly = port.registerOrUpdate(request(replacement));
    GAMENET_TEST_ASSERT(readOnly.identity == current.identity);
    const std::array removedInterestEvent{
        NativeReadinessEvent{
            .generation = current.identity.generation,
            .events = EPOLLOUT,
        },
    };
    const auto filtered = EpollReadinessPortHarness::decode(
        port,
        removedInterestEvent);
    GAMENET_TEST_ASSERT(filtered.notices.empty());

    GAMENET_TEST_ASSERT(
        port.cancel(&replacement) == ReadinessPortResult::Accepted);
    GAMENET_TEST_ASSERT(!port.has(&replacement));
}

void testBoundedWaitContinuesAndBackendWakeupIsInternal() {
    gamenet::net::EventLoop loop;
    std::array<SocketPair, 3> sockets;
    std::array<std::unique_ptr<gamenet::net::Channel>, 3> channels{
        std::make_unique<gamenet::net::Channel>(
            &loop, sockets[0].first),
        std::make_unique<gamenet::net::Channel>(
            &loop, sockets[1].first),
        std::make_unique<gamenet::net::Channel>(
            &loop, sockets[2].first),
    };
    EpollReadinessPort port(&loop, ReadinessPortOptions{
        .maxNoticesPerWait = 2,
    });
    std::unordered_set<std::uint64_t> expected;
    for (auto& channel : channels) {
        const auto registered = port.registerOrUpdate(request(*channel));
        GAMENET_TEST_ASSERT(
            registered.result == ReadinessPortResult::Accepted);
        expected.insert(registered.identity.generation);
    }

    for (const auto& socketsPair : sockets) {
        const char byte = 'x';
        GAMENET_TEST_ASSERT(
            gamenet::net::sockets::write(
                socketsPair.second,
                &byte,
                sizeof(byte)) == 1);
    }

    std::unordered_set<std::uint64_t> observed;
    bool exhausted = false;
    for (int attempt = 0; attempt < 8 && observed.size() != expected.size();
         ++attempt) {
        const auto batch = port.wait(0);
        GAMENET_TEST_ASSERT(batch.notices.size() <= 2);
        exhausted = exhausted || batch.progress.budgetExhausted;
        for (const auto& notice : batch.notices) {
            observed.insert(notice.identity.generation);
        }
    }
    GAMENET_TEST_ASSERT(exhausted);
    GAMENET_TEST_ASSERT(observed == expected);

    for (auto& channel : channels) {
        GAMENET_TEST_ASSERT(
            port.cancel(channel.get()) == ReadinessPortResult::Accepted);
    }

    bool posted = false;
    std::thread producer([&] { posted = port.wakeup(); });
    producer.join();
    GAMENET_TEST_ASSERT(posted);
    const auto wakeup = port.wait(1000);
    GAMENET_TEST_ASSERT(wakeup.notices.empty());
    GAMENET_TEST_ASSERT(wakeup.progress.wakeupNotices == 1);
}

void testLevelTriggeredNoticePreservesApplicationEagain() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    gamenet::net::Channel channel(&loop, sockets.first);
    EpollReadinessPort port(&loop);
    const auto registered = port.registerOrUpdate(request(channel));
    GAMENET_TEST_ASSERT(
        registered.result == ReadinessPortResult::Accepted);

    const std::array payload{'o', 'k'};
    GAMENET_TEST_ASSERT(
        gamenet::net::sockets::write(
            sockets.second,
            payload.data(),
            payload.size()) ==
        static_cast<ssize_t>(payload.size()));

    const auto first = port.wait(0);
    GAMENET_TEST_ASSERT(first.notices.size() == 1);
    GAMENET_TEST_ASSERT(
        first.notices[0].identity == registered.identity);
    const auto repeated = port.wait(0);
    GAMENET_TEST_ASSERT(repeated.notices.size() == 1);
    GAMENET_TEST_ASSERT(
        repeated.notices[0].identity == registered.identity);

    std::array<char, 2> received{};
    GAMENET_TEST_ASSERT(
        gamenet::net::sockets::read(
            sockets.first,
            received.data(),
            received.size()) ==
        static_cast<ssize_t>(received.size()));
    char extra = 0;
    GAMENET_TEST_ASSERT(
        gamenet::net::sockets::read(sockets.first, &extra, 1) < 0);
    GAMENET_TEST_ASSERT(
        gamenet::net::sockets::isWouldBlock(
            gamenet::net::sockets::lastError()));
    GAMENET_TEST_ASSERT(port.wait(0).notices.empty());
    GAMENET_TEST_ASSERT(
        port.cancel(&channel) == ReadinessPortResult::Accepted);
}

#endif

}  // namespace

int main() {
    testPlatformCapabilityAndInternalCapacity();
#ifndef _WIN32
    testGenerationRejectsStaleRemoveAndMergesCurrentMask();
    testBoundedWaitContinuesAndBackendWakeupIsInternal();
    testLevelTriggeredNoticePreservesApplicationEagain();
#endif
    return 0;
}
