#include "runtime_profiles/MultiIoShardedHybrid.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/protocol/PacketFramer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using gamenet::examples::ConnectionPlacementPolicy;
using gamenet::examples::HybridDispatchLane;
using gamenet::examples::LogicShardKey;
using gamenet::examples::LogicShardKeyKind;
using gamenet::examples::LogicShardPolicy;
using gamenet::examples::MultiIoShardedHybrid;
using gamenet::examples::MultiIoShardedHybridAction;
using gamenet::examples::MultiIoShardedHybridContext;
using gamenet::examples::MultiIoShardedHybridHandlerResult;
using gamenet::examples::MultiIoShardedHybridMetrics;
using gamenet::examples::MultiIoShardedHybridOptions;
using gamenet::examples::MultiIoShardedHybridRoute;
using gamenet::examples::MultiIoShardedHybridStopHandle;

struct ParsedPayload {
    HybridDispatchLane lane{HybridDispatchLane::EventDriven};
    LogicShardKey key;
    std::string_view body;
};

ParsedPayload parsePayload(std::string_view payload) {
    const auto first = payload.find('|');
    const auto second = first == std::string_view::npos
        ? std::string_view::npos
        : payload.find('|', first + 1);
    const auto third = second == std::string_view::npos
        ? std::string_view::npos
        : payload.find('|', second + 1);
    if (first != 1 || second == std::string_view::npos ||
        third == std::string_view::npos || second == first + 1 ||
        third == second + 1 || third + 1 >= payload.size()) {
        throw std::invalid_argument("invalid sharded test payload");
    }
    LogicShardKeyKind kind{};
    switch (payload[first + 1]) {
    case 'p': kind = LogicShardKeyKind::Player; break;
    case 'r': kind = LogicShardKeyKind::Room; break;
    case 's': kind = LogicShardKeyKind::Scene; break;
    default: throw std::invalid_argument("invalid sharded test key kind");
    }
    return {
        .lane = payload.front() == 'f'
            ? HybridDispatchLane::FixedTick
            : HybridDispatchLane::EventDriven,
        .key = {
            .kind = kind,
            .value = std::string(payload.substr(second + 1, third - second - 1)),
        },
        .body = payload.substr(third + 1),
    };
}

MultiIoShardedHybridRoute routePayload(
    gamenet::transport::TransportSessionId,
    std::string_view payload) {
    const auto parsed = parsePayload(payload);
    return {.key = parsed.key, .lane = parsed.lane};
}

MultiIoShardedHybridOptions profileOptions() {
    return {
        .ioThreads = 2,
        .connectionPlacement = ConnectionPlacementPolicy::RoundRobin,
        .logicShardPolicy = LogicShardPolicy::StableHash,
        .framing = {
            .maxPayloadBytes = 512,
            .maxBufferedBytes = 4096,
            .maxFramesPerPush = 32,
            .maxFrameBytesPerPush = 4096,
            .maxRetainedCapacityBytes = 516,
            .trimThresholdBytes = 256,
        },
        .maxCommandsPerCell = 64,
        .maxQueuedBytesPerCell = 64U * 512U,
        .maxPayloadBytes = 512,
        .maxShardKeyBytes = 64,
        .maxCommandsPerEventDrain = 2,
        .maxCommandsPerTick = 4,
        .tickInterval = 100ms,
        .admission = {.maxConnections = 16},
        .connectionBackpressure = {
            .lowWaterMarkBytes = 1024,
            .highWaterMarkBytes = 2048,
            .hardLimitBytes = 4096,
            .maxInputBufferBytes = 4096,
        },
        .maxRouterWallTime = 5s,
        .maxHandlerWallTime = 5s,
        .maxTurnWallTime = 5s,
    };
}

std::string encodeFrames(
    const gamenet::protocol::PacketFramerOptions& options,
    const std::vector<std::string>& frames) {
    gamenet::protocol::PacketFramer encoder(options);
    std::string bytes;
    for (const auto& frame : frames) {
        const auto encoded = encoder.encode(frame);
        GAMENET_TEST_ASSERT(encoded.has_value());
        bytes += *encoded;
    }
    return bytes;
}

bool writeEventually(gamenet::net::SocketFd fd, std::string_view bytes) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (offset != bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const auto written = gamenet::net::sockets::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        const auto error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInProgress(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return offset == bytes.size();
}

std::vector<std::string> readFramesEventually(
    gamenet::net::SocketFd fd,
    const gamenet::protocol::PacketFramerOptions& options,
    std::size_t expectedCount) {
    gamenet::protocol::PacketFramer decoder(options);
    std::vector<std::string> received;
    std::array<char, 2048> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while (received.size() < expectedCount &&
           std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(fd, bytes.data(), bytes.size());
        if (count > 0) {
            auto result = decoder.push(
                std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (true) {
                for (auto& frame : result.frames) {
                    received.push_back(std::move(frame));
                }
                if (!result.needsContinuation) break;
                result = decoder.push({});
            }
            continue;
        }
        if (count == 0) break;
        const auto error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    return received;
}

bool waitForClose(gamenet::net::SocketFd fd) {
    std::array<char, 64> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(fd, bytes.data(), bytes.size());
        if (count == 0) return true;
        if (count < 0) {
            const auto error = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isWouldBlock(error) &&
                !gamenet::net::sockets::isInterrupted(error)) {
                return true;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::steady_clock::duration timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

std::pair<LogicShardKey, LogicShardKey> keysForDifferentCells(
    const MultiIoShardedHybrid& profile) {
    std::optional<LogicShardKey> first;
    std::size_t firstCell = 0;
    for (std::size_t index = 0; index < 1000; ++index) {
        LogicShardKey key{
            .kind = LogicShardKeyKind::Room,
            .value = "room-" + std::to_string(index),
        };
        const auto cell = profile.logicShardForKey(key);
        if (!first) {
            first = key;
            firstCell = cell;
        } else if (cell != firstCell) {
            return {*first, std::move(key)};
        }
    }
    throw std::runtime_error("failed to find distinct stable-hash cells");
}

std::string command(
    HybridDispatchLane lane,
    const LogicShardKey& key,
    std::string_view body) {
    const char kind = key.kind == LogicShardKeyKind::Player
        ? 'p'
        : key.kind == LogicShardKeyKind::Room ? 'r' : 's';
    return std::string(lane == HybridDispatchLane::FixedTick ? "f|" : "e|") +
        kind + "|" + key.value + "|" + std::string(body);
}

void invalidConfigurationIsRejected() {
    gamenet::net::EventLoop baseLoop;
    gamenet::net::EventLoopThread firstThread;
    gamenet::net::EventLoopThread secondThread;
    auto* first = firstThread.startLoop();
    auto* second = secondThread.startLoop();

    auto expectRejected = [&](
                              MultiIoShardedHybridOptions options,
                              std::vector<gamenet::net::EventLoop*> loops,
                              bool emptyRouter = false) {
        bool rejected = false;
        try {
            MultiIoShardedHybrid invalid(
                &baseLoop,
                std::move(loops),
                gamenet::net::InetAddress(0, true),
                emptyRouter ? gamenet::examples::MultiIoShardedHybridRouter{}
                            : gamenet::examples::MultiIoShardedHybridRouter{routePayload},
                [](const MultiIoShardedHybridContext&, auto, auto) {
                    return MultiIoShardedHybridHandlerResult{};
                },
                options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GAMENET_TEST_ASSERT(rejected);
    };

    auto options = profileOptions();
    options.ioThreads = 1;
    expectRejected(options, {first, second});
    options = profileOptions();
    options.maxCommandsPerCell = 0;
    expectRejected(options, {first, second});
    options = profileOptions();
    options.tickInterval = 0ms;
    expectRejected(options, {first, second});
    expectRejected(profileOptions(), {first});
    expectRejected(profileOptions(), {first, first});
    expectRejected(profileOptions(), {first, &baseLoop});
    expectRejected(profileOptions(), {first, second}, true);

    secondThread.stop();
    firstThread.stop();
}

struct Observation {
    std::string body;
    std::size_t shard{};
    std::uint64_t sequence{};
    std::uint64_t networkOwner{};
    std::uint64_t logicOwner{};
    gamenet::transport::TransportSessionId transportId{};
    HybridDispatchLane lane{HybridDispatchLane::EventDriven};
};

void independentPlacementPreservesCellOrderAndHybridProgress() {
    gamenet::net::EventLoopThread firstThread;
    gamenet::net::EventLoopThread secondThread;
    auto* first = firstThread.startLoop();
    auto* second = secondThread.startLoop();

    std::promise<void> releaseFirstPromise;
    std::promise<void> releaseSecondPromise;
    auto releaseFirst = releaseFirstPromise.get_future().share();
    auto releaseSecond = releaseSecondPromise.get_future().share();
    std::atomic<int> blocked{0};
    GAMENET_TEST_ASSERT(first->executor().post([&] {
        blocked.fetch_add(1, std::memory_order_release);
        releaseFirst.wait();
    }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(second->executor().post([&] {
        blocked.fetch_add(1, std::memory_order_release);
        releaseSecond.wait();
    }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(waitUntil([&] { return blocked.load() == 2; }));

    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();
    std::mutex observationsMutex;
    std::vector<Observation> observations;
    auto profile = std::make_unique<MultiIoShardedHybrid>(
        &baseLoop,
        std::vector<gamenet::net::EventLoop*>{first, second},
        gamenet::net::InetAddress(0, true),
        routePayload,
        [&](const MultiIoShardedHybridContext& context,
            auto transportId,
            std::string_view payload) {
            const auto parsed = parsePayload(payload);
            std::lock_guard lock(observationsMutex);
            observations.push_back({
                .body = std::string(parsed.body),
                .shard = context.logicShardIndex,
                .sequence = context.cellSequence,
                .networkOwner = context.connectionOwnerExecutorId,
                .logicOwner = context.logicOwnerExecutorId,
                .transportId = transportId,
                .lane = context.lane,
            });
            return MultiIoShardedHybridHandlerResult{
                .reply = std::string(payload),
            };
        },
        options);
    const auto [keyA, keyB] = keysForDifferentCells(*profile);
    const auto cellA = profile->logicShardForKey(keyA);
    const auto cellB = profile->logicShardForKey(keyB);
    GAMENET_TEST_ASSERT(cellA != cellB);
    profile->start();

    std::atomic<bool> readyToRelease{false};
    std::atomic<bool> releaseIssued{false};
    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto firstClient = gamenet::test::connectTestClient(profile->listenAddress());
        const auto secondClient = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = waitUntil([&] { return profile->metrics().networkOwnerCount == 2; });
        const std::vector<std::string> firstFrames{
            command(HybridDispatchLane::EventDriven, keyA, "A"),
            command(HybridDispatchLane::FixedTick, keyA, "B"),
            command(HybridDispatchLane::EventDriven, keyA, "C"),
            command(HybridDispatchLane::EventDriven, keyB, "X"),
        };
        const std::vector<std::string> secondFrames{
            command(HybridDispatchLane::EventDriven, keyA, "D"),
        };
        ok = ok && writeEventually(
            firstClient, encodeFrames(options.framing, firstFrames));
        ok = ok && writeEventually(
            secondClient, encodeFrames(options.framing, secondFrames));
        ok = ok && waitUntil([&] {
            return profile->metrics().commandsAccepted == 5;
        });
        readyToRelease.store(true, std::memory_order_release);
        ok = ok && waitUntil([&] { return releaseIssued.load(); });
        auto firstReplies = readFramesEventually(firstClient, options.framing, 4);
        auto secondReplies = readFramesEventually(secondClient, options.framing, 1);
        std::sort(firstReplies.begin(), firstReplies.end());
        auto expectedFirst = firstFrames;
        std::sort(expectedFirst.begin(), expectedFirst.end());
        ok = ok && firstReplies == expectedFirst && secondReplies == secondFrames;
        gamenet::test::closeTestSocket(firstClient);
        gamenet::test::closeTestSocket(secondClient);
        clientSucceeded.store(ok);
        clientDone.store(true, std::memory_order_release);
    });

    MultiIoShardedHybridStopHandle stop;
    bool noHandlerBeforeRelease = false;
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        if (readyToRelease.load(std::memory_order_acquire) &&
            !releaseIssued.exchange(true, std::memory_order_acq_rel)) {
            noHandlerBeforeRelease = profile->metrics().handlerCalls == 0;
            releaseFirstPromise.set_value();
            releaseSecondPromise.set_value();
            return;
        }
        if (clientDone.load(std::memory_order_acquire) && !stop.valid()) {
            stop = profile->stopGracefully({.drainTimeout = 0ms});
            return;
        }
        if (stop.valid() &&
            stop.networkStop.wait_for(0s) == std::future_status::ready &&
            stop.logicStop.wait_for(0s) == std::future_status::ready) {
            baseLoop.quit();
        }
    }));
    gamenet::test::runLoopWithTimeout(baseLoop, 10s, "sharded Hybrid ordering timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    GAMENET_TEST_ASSERT(noHandlerBeforeRelease);
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.networkOwnerCount == 2);
    GAMENET_TEST_ASSERT(metrics.logicCellCount == 2);
    GAMENET_TEST_ASSERT(metrics.connectionOwnerMigrations == 0);
    GAMENET_TEST_ASSERT(metrics.orderViolations == 0);
    GAMENET_TEST_ASSERT(metrics.fixedHeadDeferrals >= 1);
    GAMENET_TEST_ASSERT(metrics.eventCommandsAccepted == 4);
    GAMENET_TEST_ASSERT(metrics.fixedCommandsAccepted == 1);
    GAMENET_TEST_ASSERT(metrics.crossDomainHandoffs == 10);

    std::lock_guard lock(observationsMutex);
    std::map<std::size_t, std::uint64_t> lastSequence;
    std::set<std::uint64_t> keyAOwners;
    std::map<std::uint64_t, std::set<std::size_t>> transportShards;
    std::map<std::uint64_t, std::uint64_t> transportOwners;
    std::size_t indexA = observations.size();
    std::size_t indexB = observations.size();
    std::size_t indexC = observations.size();
    std::size_t indexX = observations.size();
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& item = observations[index];
        GAMENET_TEST_ASSERT(item.sequence > lastSequence[item.shard]);
        lastSequence[item.shard] = item.sequence;
        transportShards[item.transportId.value].insert(item.shard);
        const auto [ownerIt, inserted] = transportOwners.emplace(
            item.transportId.value, item.networkOwner);
        GAMENET_TEST_ASSERT(inserted || ownerIt->second == item.networkOwner);
        if (item.shard == cellA) keyAOwners.insert(item.networkOwner);
        if (item.body == "A") indexA = index;
        if (item.body == "B") indexB = index;
        if (item.body == "C") indexC = index;
        if (item.body == "X") indexX = index;
    }
    GAMENET_TEST_ASSERT(observations.size() == 5);
    GAMENET_TEST_ASSERT(indexA < indexB && indexB < indexC);
    GAMENET_TEST_ASSERT(indexX < indexB);
    GAMENET_TEST_ASSERT(keyAOwners.size() == 2);
    GAMENET_TEST_ASSERT(std::any_of(
        transportShards.begin(), transportShards.end(),
        [](const auto& entry) { return entry.second.size() == 2; }));

    profile.reset();
    secondThread.stop();
    firstThread.stop();
}

void perCellSaturationIsIsolated() {
    gamenet::net::EventLoopThread firstThread;
    gamenet::net::EventLoopThread secondThread;
    auto* first = firstThread.startLoop();
    auto* second = secondThread.startLoop();

    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();
    options.maxCommandsPerCell = 2;
    auto profile = std::make_unique<MultiIoShardedHybrid>(
        &baseLoop,
        std::vector<gamenet::net::EventLoop*>{first, second},
        gamenet::net::InetAddress(0, true),
        routePayload,
        [](const MultiIoShardedHybridContext&, auto, std::string_view payload) {
            return MultiIoShardedHybridHandlerResult{.reply = std::string(payload)};
        },
        options);
    const auto [candidateA, candidateB] = keysForDifferentCells(*profile);
    const auto keyBlocked = profile->logicShardForKey(candidateA) == 0
        ? candidateA : candidateB;
    const auto keyHealthy = profile->logicShardForKey(candidateA) == 1
        ? candidateA : candidateB;

    std::promise<void> releaseBlockedPromise;
    auto releaseBlocked = releaseBlockedPromise.get_future().share();
    std::atomic<bool> blocked{false};
    GAMENET_TEST_ASSERT(first->executor().post([&] {
        blocked.store(true, std::memory_order_release);
        releaseBlocked.wait();
    }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(waitUntil([&] { return blocked.load(); }));
    profile->start();

    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto victim = gamenet::test::connectTestClient(profile->listenAddress());
        const auto healthy = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = waitUntil([&] { return profile->metrics().networkOwnerCount == 2; });
        const std::vector<std::string> victimFrames{
            command(HybridDispatchLane::FixedTick, keyBlocked, "one"),
            command(HybridDispatchLane::FixedTick, keyBlocked, "two"),
            command(HybridDispatchLane::FixedTick, keyBlocked, "overflow"),
        };
        const auto healthyFrame = command(
            HybridDispatchLane::EventDriven, keyHealthy, "healthy");
        ok = ok && writeEventually(victim, encodeFrames(options.framing, victimFrames));
        ok = ok && writeEventually(
            healthy, encodeFrames(options.framing, {healthyFrame}));
        ok = ok && waitForClose(victim);
        ok = ok && readFramesEventually(healthy, options.framing, 1) ==
            std::vector<std::string>{healthyFrame};
        releaseBlockedPromise.set_value();
        ok = ok && waitUntil([&] { return profile->metrics().staleInputs >= 2; });
        gamenet::test::closeTestSocket(victim);
        gamenet::test::closeTestSocket(healthy);
        clientSucceeded.store(ok);
        clientDone.store(true, std::memory_order_release);
    });

    MultiIoShardedHybridStopHandle stop;
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        if (clientDone.load(std::memory_order_acquire) && !stop.valid()) {
            stop = profile->stopGracefully({.drainTimeout = 0ms});
            return;
        }
        if (stop.valid() &&
            stop.networkStop.wait_for(0s) == std::future_status::ready &&
            stop.logicStop.wait_for(0s) == std::future_status::ready) {
            baseLoop.quit();
        }
    }));
    gamenet::test::runLoopWithTimeout(baseLoop, 10s, "sharded saturation timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.queueFullRejections >= 1);
    GAMENET_TEST_ASSERT(metrics.staleInputs >= 2);
    GAMENET_TEST_ASSERT(metrics.outputsAccepted == 1);
    GAMENET_TEST_ASSERT(metrics.profileTerminalFailures == 0);
    GAMENET_TEST_ASSERT(metrics.cells[0].depth == 0);
    GAMENET_TEST_ASSERT(metrics.cells[1].depth == 0);
    profile.reset();
    secondThread.stop();
    firstThread.stop();
}

void activeHandlerDelaysAggregateLogicStop() {
    gamenet::net::EventLoopThread firstThread;
    gamenet::net::EventLoopThread secondThread;
    auto* first = firstThread.startLoop();
    auto* second = secondThread.startLoop();
    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();

    std::promise<void> releaseHandlerPromise;
    auto releaseHandler = releaseHandlerPromise.get_future().share();
    std::atomic<bool> handlerEntered{false};
    auto profile = std::make_unique<MultiIoShardedHybrid>(
        &baseLoop,
        std::vector<gamenet::net::EventLoop*>{first, second},
        gamenet::net::InetAddress(0, true),
        routePayload,
        [&](const MultiIoShardedHybridContext&, auto, std::string_view payload) {
            if (parsePayload(payload).body == "hold") {
                handlerEntered.store(true, std::memory_order_release);
                releaseHandler.wait();
            }
            return MultiIoShardedHybridHandlerResult{.reply = "must-not-send"};
        },
        options);
    const auto key = keysForDifferentCells(*profile).first;
    profile->start();

    std::atomic<bool> stopIssued{false};
    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto fd = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = writeEventually(
            fd,
            encodeFrames(options.framing, {
                command(HybridDispatchLane::EventDriven, key, "hold"),
                command(HybridDispatchLane::FixedTick, key, "cancel-me"),
            })) && waitUntil([&] { return stopIssued.load(); });
        releaseHandlerPromise.set_value();
        ok = ok && waitForClose(fd);
        gamenet::test::closeTestSocket(fd);
        clientSucceeded.store(ok);
        clientDone.store(true, std::memory_order_release);
    });

    MultiIoShardedHybridStopHandle stop;
    bool logicFutureWasPending = false;
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        if (handlerEntered.load(std::memory_order_acquire) && !stop.valid()) {
            stop = profile->stopGracefully({.drainTimeout = 0ms});
            logicFutureWasPending =
                stop.logicStop.wait_for(0s) != std::future_status::ready;
            stopIssued.store(true, std::memory_order_release);
            return;
        }
        if (!clientDone.load(std::memory_order_acquire) || !stop.valid()) return;
        if (stop.networkStop.wait_for(0s) == std::future_status::ready &&
            stop.logicStop.wait_for(0s) == std::future_status::ready) {
            baseLoop.quit();
        }
    }));
    gamenet::test::runLoopWithTimeout(baseLoop, 10s, "sharded active stop timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    GAMENET_TEST_ASSERT(logicFutureWasPending);
    GAMENET_TEST_ASSERT(stop.logicStop.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(stop.networkStop.wait_for(0s) == std::future_status::ready);
    const auto summary = stop.logicStop.get();
    GAMENET_TEST_ASSERT(summary.droppedCommands >= 1);
    GAMENET_TEST_ASSERT(summary.activeCallbacksObserved);
    GAMENET_TEST_ASSERT(!summary.terminalCellFailure);
    GAMENET_TEST_ASSERT(summary.cellsRetired == 2);
    GAMENET_TEST_ASSERT(summary.shutdownDrainWaitUs > 0);
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.staleOutputs >= 1);
    GAMENET_TEST_ASSERT(metrics.outputsAccepted == 0);
    GAMENET_TEST_ASSERT(metrics.logicStopDroppedCommands >= 1);
    profile.reset();
    secondThread.stop();
    firstThread.stop();
}

}  // namespace

int main() {
    invalidConfigurationIsRejected();
    independentPlacementPreservesCellOrderAndHybridProgress();
    perCellSaturationIsIsolated();
    activeHandlerDelaysAggregateLogicStop();
    return 0;
}
