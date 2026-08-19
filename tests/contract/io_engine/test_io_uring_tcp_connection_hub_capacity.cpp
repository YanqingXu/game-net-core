#include "experimental/io_uring/IoUringTcpConnectionHub.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

namespace uring = gamenet::experimental::io_uring;

constexpr std::size_t kInitialConnections = 256;
constexpr std::size_t kReplacementConnections = 64;
constexpr std::size_t kRetainedConnections =
    kInitialConnections - kReplacementConnections;

class OwnedFd {
public:
    explicit OwnedFd(int value = -1) noexcept : value_(value) {}
    ~OwnedFd() {
        if (value_ >= 0) ::close(value_);
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this == &other) return *this;
        if (value_ >= 0) ::close(value_);
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }

private:
    int value_;
};

struct SocketPair {
    OwnedFd hub;
    OwnedFd peer;
};

class TcpPairFactory {
public:
    TcpPairFactory()
        : listener_(::socket(
              AF_INET,
              SOCK_STREAM | SOCK_CLOEXEC,
              IPPROTO_TCP)) {
        GAMENET_TEST_ASSERT(listener_.get() >= 0);
        int enabled = 1;
        GAMENET_TEST_ASSERT(
            ::setsockopt(
                listener_.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                &enabled,
                sizeof(enabled)) == 0);
        address_.sin_family = AF_INET;
        address_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_.sin_port = 0;
        GAMENET_TEST_ASSERT(
            ::bind(
                listener_.get(),
                reinterpret_cast<const sockaddr*>(&address_),
                sizeof(address_)) == 0);
        GAMENET_TEST_ASSERT(
            ::listen(
                listener_.get(),
                static_cast<int>(kInitialConnections +
                                 kReplacementConnections)) == 0);
        socklen_t length = sizeof(address_);
        GAMENET_TEST_ASSERT(
            ::getsockname(
                listener_.get(),
                reinterpret_cast<sockaddr*>(&address_),
                &length) == 0);
    }

    SocketPair makePair() {
        OwnedFd peer(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_CLOEXEC,
            IPPROTO_TCP));
        GAMENET_TEST_ASSERT(peer.get() >= 0);
        GAMENET_TEST_ASSERT(
            ::connect(
                peer.get(),
                reinterpret_cast<const sockaddr*>(&address_),
                sizeof(address_)) == 0);
        OwnedFd hub(::accept4(
            listener_.get(),
            nullptr,
            nullptr,
            SOCK_CLOEXEC));
        GAMENET_TEST_ASSERT(hub.get() >= 0);
        return {.hub = std::move(hub), .peer = std::move(peer)};
    }

private:
    OwnedFd listener_;
    sockaddr_in address_{};
};

void sendByte(int descriptor, char value) {
    GAMENET_TEST_ASSERT(
        ::send(descriptor, &value, 1, MSG_NOSIGNAL) == 1);
}

std::string readToEof(int descriptor) {
    std::string result;
    std::array<char, 8> buffer{};
    while (true) {
        const auto count = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            result.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        GAMENET_TEST_ASSERT(count == 0);
        return result;
    }
}

uring::IoUringTcpConnectionHubOptions capacityOptions() {
    return {
        .pump = {
            .engine = {
                .entries = 1024,
                .maxOperations = 512,
                .maxCompletionsPerWait = 512,
                .maxBytesPerOperation = 1,
                .maxOwnedBytes = 1024,
            },
            .maxNoticesPerTurn = 64,
        },
        .maxConnections = kInitialConnections,
        .maxTotalPendingSendBytes = kInitialConnections,
        .maxReceiveBytes = 1,
        .maxSendBytesPerOperation = 1,
        .maxPendingSendBytesPerConnection = 2,
        .maxPendingSendSegmentsPerConnection = 2,
    };
}

void testFixed256RouteChurnAndPostReplacementProgress() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 16,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 4,
        .maxControlSources = 0,
        .maxLifecycleNodes = 2,
        .maxLifecycleCallbacksPerIteration = 2,
        .maxActiveChannelsPerIteration = 1,
    });
    TcpPairFactory factory;
    std::vector<SocketPair> initialPairs;
    std::vector<SocketPair> replacementPairs;
    initialPairs.reserve(kInitialConnections);
    replacementPairs.reserve(kReplacementConnections);
    for (std::size_t index = 0; index < kInitialConnections; ++index) {
        initialPairs.push_back(factory.makePair());
    }
    for (std::size_t index = 0; index < kReplacementConnections; ++index) {
        replacementPairs.push_back(factory.makePair());
    }

    std::vector<uring::IoUringTcpConnectionIdentity> initialIdentities(
        kInitialConnections);
    std::vector<uring::IoUringTcpConnectionIdentity> replacementIdentities(
        kReplacementConnections);
    std::vector<
        std::shared_future<
            uring::IoUringTcpConnectionHubConnectionStopSummary>>
        initialFutures(kInitialConnections);
    std::vector<
        std::shared_future<
            uring::IoUringTcpConnectionHubConnectionStopSummary>>
        replacementFutures(kReplacementConnections);
    std::vector<std::size_t> retainedMessages(kInitialConnections);
    std::size_t oldCloseCalls = 0;
    std::size_t retainedCloseCalls = 0;
    std::size_t replacementCloseCalls = 0;
    std::size_t postReplacementMessages = 0;
    bool replacementBegan = false;
    bool closeScheduled = false;
    bool timedOut = false;
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;

    auto stopWhenAllRetired = [&] {
        if (oldCloseCalls + retainedCloseCalls + replacementCloseCalls ==
            kInitialConnections + kReplacementConnections) {
            GAMENET_TEST_ASSERT(hubPointer->stop());
        }
    };

    auto scheduleAggregateClose = [&] {
        if (closeScheduled ||
            postReplacementMessages != kInitialConnections) {
            return;
        }
        closeScheduled = true;
        loop.runAfter(25ms, [&] {
            const auto beforeClose = hubPointer->metrics();
            GAMENET_TEST_ASSERT(
                beforeClose.bytesSent == kInitialConnections * 2U);
            GAMENET_TEST_ASSERT(beforeClose.pendingSendBytes == 0);
            for (std::size_t index = kReplacementConnections;
                 index < kInitialConnections;
                 ++index) {
                GAMENET_TEST_ASSERT(
                    hubPointer->closeConnection(initialIdentities[index]));
            }
            for (const auto identity : replacementIdentities) {
                GAMENET_TEST_ASSERT(hubPointer->closeConnection(identity));
            }
        });
    };

    uring::IoUringTcpConnectionHub hub(
        &loop,
        capacityOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            GAMENET_TEST_ASSERT(summary.allConnectionsStopped);
            GAMENET_TEST_ASSERT(summary.hub.activeConnections == 0);
            GAMENET_TEST_ASSERT(summary.hub.activeOperationRoutes == 0);
            GAMENET_TEST_ASSERT(summary.hub.pendingSendBytes == 0);
            loop.quit();
        });
    hubPointer = &hub;

    for (std::size_t index = 0; index < kInitialConnections; ++index) {
        const auto added = hub.addConnection(
            initialPairs[index].hub.release(),
            [&, index](
                uring::IoUringTcpConnectionIdentity identity,
                std::string_view payload) {
                GAMENET_TEST_ASSERT(identity == initialIdentities[index]);
                if (index < kReplacementConnections) {
                    GAMENET_TEST_ASSERT(payload == "o");
                    GAMENET_TEST_ASSERT(hub.closeConnection(identity));
                    return;
                }
                ++retainedMessages[index];
                if (retainedMessages[index] == 1) {
                    GAMENET_TEST_ASSERT(payload == "i");
                    return;
                }
                GAMENET_TEST_ASSERT(retainedMessages[index] == 2);
                GAMENET_TEST_ASSERT(replacementBegan);
                GAMENET_TEST_ASSERT(payload == "n");
                GAMENET_TEST_ASSERT(
                    hub.send(identity, "N") ==
                    uring::IoUringTcpHubSendResult::Accepted);
                ++postReplacementMessages;
                scheduleAggregateClose();
            },
            [&, index](
                uring::IoUringTcpConnectionIdentity identity,
                uring::IoUringTcpHubCloseReason reason) {
                GAMENET_TEST_ASSERT(identity == initialIdentities[index]);
                GAMENET_TEST_ASSERT(
                    reason == uring::IoUringTcpHubCloseReason::Explicit);
                GAMENET_TEST_ASSERT(
                    initialFutures[index].wait_for(0s) ==
                    std::future_status::ready);
                if (index >= kReplacementConnections) {
                    ++retainedCloseCalls;
                    stopWhenAllRetired();
                    return;
                }

                ++oldCloseCalls;
                const auto replacement = hub.addConnection(
                    replacementPairs[index].hub.release(),
                    [&, index](
                        uring::IoUringTcpConnectionIdentity current,
                        std::string_view replacementPayload) {
                        GAMENET_TEST_ASSERT(
                            current == replacementIdentities[index]);
                        GAMENET_TEST_ASSERT(replacementBegan);
                        GAMENET_TEST_ASSERT(replacementPayload == "r");
                        GAMENET_TEST_ASSERT(
                            hub.send(current, "R") ==
                            uring::IoUringTcpHubSendResult::Accepted);
                        ++postReplacementMessages;
                        scheduleAggregateClose();
                    },
                    [&, index](
                        uring::IoUringTcpConnectionIdentity current,
                        uring::IoUringTcpHubCloseReason closeReason) {
                        GAMENET_TEST_ASSERT(
                            current == replacementIdentities[index]);
                        GAMENET_TEST_ASSERT(
                            closeReason ==
                            uring::IoUringTcpHubCloseReason::Explicit);
                        ++replacementCloseCalls;
                        stopWhenAllRetired();
                    });
                GAMENET_TEST_ASSERT(
                    replacement.result ==
                    uring::IoUringTcpHubAddResult::Accepted);
                replacementIdentities[index] = replacement.identity;
                replacementFutures[index] = replacement.stopFuture;
                GAMENET_TEST_ASSERT(
                    replacement.identity.slot ==
                    initialIdentities[index].slot);
                GAMENET_TEST_ASSERT(
                    replacement.identity.generation !=
                    initialIdentities[index].generation);
                GAMENET_TEST_ASSERT(
                    hub.send(initialIdentities[index], "x") ==
                    uring::IoUringTcpHubSendResult::StaleConnection);
                GAMENET_TEST_ASSERT(
                    !hub.closeConnection(initialIdentities[index]));

                if (oldCloseCalls == kReplacementConnections) {
                    replacementBegan = true;
                    for (std::size_t retained = kReplacementConnections;
                         retained < kInitialConnections;
                         ++retained) {
                        sendByte(initialPairs[retained].peer.get(), 'n');
                    }
                    for (auto& pair : replacementPairs) {
                        sendByte(pair.peer.get(), 'r');
                    }
                }
                stopWhenAllRetired();
            });
        GAMENET_TEST_ASSERT(
            added.result == uring::IoUringTcpHubAddResult::Accepted);
        initialIdentities[index] = added.identity;
        initialFutures[index] = added.stopFuture;
    }

    GAMENET_TEST_ASSERT(hub.metrics().activeConnections == kInitialConnections);
    for (const auto identity : initialIdentities) {
        GAMENET_TEST_ASSERT(
            hub.send(identity, "S") ==
            uring::IoUringTcpHubSendResult::Accepted);
    }
    GAMENET_TEST_ASSERT(
        hub.send(initialIdentities.front(), "x") ==
        uring::IoUringTcpHubSendResult::HubByteLimit);
    for (std::size_t index = 0; index < kInitialConnections; ++index) {
        sendByte(
            initialPairs[index].peer.get(),
            index < kReplacementConnections ? 'o' : 'i');
    }
    loop.runAfter(5s, [&] {
        timedOut = true;
        (void)hub.stop();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(!timedOut);
    GAMENET_TEST_ASSERT(oldCloseCalls == kReplacementConnections);
    GAMENET_TEST_ASSERT(retainedCloseCalls == kRetainedConnections);
    GAMENET_TEST_ASSERT(replacementCloseCalls == kReplacementConnections);
    GAMENET_TEST_ASSERT(postReplacementMessages == kInitialConnections);
    for (const auto& future : initialFutures) {
        GAMENET_TEST_ASSERT(future.wait_for(0s) == std::future_status::ready);
    }
    for (const auto& future : replacementFutures) {
        GAMENET_TEST_ASSERT(future.wait_for(0s) == std::future_status::ready);
    }
    for (std::size_t index = 0; index < kReplacementConnections; ++index) {
        GAMENET_TEST_ASSERT(readToEof(initialPairs[index].peer.get()) == "S");
    }
    for (std::size_t index = kReplacementConnections;
         index < kInitialConnections;
         ++index) {
        GAMENET_TEST_ASSERT(readToEof(initialPairs[index].peer.get()) == "SN");
    }
    for (auto& pair : replacementPairs) {
        GAMENET_TEST_ASSERT(readToEof(pair.peer.get()) == "R");
    }

    const auto summary = hub.stopFuture().get();
    GAMENET_TEST_ASSERT(summary.hub.connectionsAccepted == 320);
    GAMENET_TEST_ASSERT(summary.hub.connectionsRetired == 320);
    GAMENET_TEST_ASSERT(summary.hub.maxActiveConnections == 256);
    GAMENET_TEST_ASSERT(summary.hub.sendAdmissions == 512);
    GAMENET_TEST_ASSERT(summary.hub.bytesSent == 512);
    GAMENET_TEST_ASSERT(summary.hub.bytesDiscarded == 0);
    GAMENET_TEST_ASSERT(summary.hub.hubByteLimitRejections == 1);
    GAMENET_TEST_ASSERT(summary.hub.staleConnectionRejections == 128);
    GAMENET_TEST_ASSERT(summary.hub.socketCloseCount == 320);
    GAMENET_TEST_ASSERT(summary.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.ownedBytes == 0);
}

}  // namespace

int main() {
    testFixed256RouteChurnAndPostReplacementProgress();
    return 0;
}
