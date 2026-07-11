#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/game_logic/LogicLoop.h"
#include "gamenet/game_session/PlayerSession.h"
#include "gamenet/protocol/PacketFramer.h"
#include "gamenet/transport/TransportEndpoint.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr double kBytesPerMebibyte = 1024.0 * 1024.0;

struct Config {
    std::string scenario{"framing"};
    std::size_t messages{100000};
    std::size_t payloadBytes{256};
    std::size_t threads{2};
    std::size_t batchSize{64};
    std::size_t fanout{1024};
    std::chrono::microseconds tickInterval{1000};
    std::chrono::milliseconds timeout{30000};
};

struct Result {
    double elapsedMs{};
    std::optional<double> throughputMibPerSecond;
    std::optional<double> operationsPerSecond;
    std::optional<double> logicQueueLagP50Us;
    std::optional<double> logicQueueLagP99Us;
    std::optional<std::uint64_t> logicDepthHighWatermark;
    std::optional<std::uint64_t> logicBytesHighWatermark;
    std::optional<std::uint64_t> logicAccepted;
    std::optional<std::uint64_t> logicRejected;
    std::optional<double> broadcastEndpointLatencyP50Us;
    std::optional<double> broadcastEndpointLatencyP99Us;
    std::optional<double> broadcastRouteLatencyP50Us;
    std::optional<double> broadcastRouteLatencyP99Us;
    std::optional<double> broadcastQueueLatencyP50Us;
    std::optional<double> broadcastQueueLatencyP99Us;
    std::optional<double> broadcastFanoutCompletionP50Us;
    std::optional<double> broadcastFanoutCompletionP99Us;
    std::optional<std::uint64_t> broadcastAccepted;
    std::optional<std::uint64_t> broadcastDropped;
    std::optional<std::uint64_t> broadcastScheduledTasks;
    std::optional<std::uint64_t> broadcastTaskHighWatermark;
    std::optional<std::uint64_t> workingSetBeforeBytes;
    std::optional<std::uint64_t> workingSetAfterBytes;
    std::optional<std::uint64_t> workingSetPeakBytes;
    std::optional<std::int64_t> workingSetPeakDeltaBytes;
};

class HelpRequested final : public std::exception {};

[[noreturn]] void usageError(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

std::uint64_t parseUnsigned(std::string_view text, std::string_view option) {
    if (text.empty()) usageError(std::string(option) + " requires a value");
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') usageError(std::string(option) + " requires an unsigned integer");
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            usageError(std::string(option) + " is too large");
        }
        value = value * 10U + digit;
    }
    return value;
}

std::size_t parseSize(
    std::string_view text,
    std::string_view option,
    std::size_t minimum,
    std::size_t maximum) {
    const auto value = parseUnsigned(text, option);
    if (value < minimum || value > maximum) {
        usageError(
            std::string(option) + " must be between " + std::to_string(minimum) + " and " +
            std::to_string(maximum));
    }
    return static_cast<std::size_t>(value);
}

void printUsage(std::ostream& output) {
    output
        << "Usage: gamenet_phase4_benchmark [options]\n"
        << "  --scenario framing|logic-queue|broadcast-fanout\n"
        << "  --messages N       frames, commands, or fanout iterations\n"
        << "  --payload N        payload bytes per item\n"
        << "  --threads N        logic producers or broadcast owner loops\n"
        << "  --batch N          frames/push, commands/tick, or endpoints/task\n"
        << "  --fanout N         broadcast endpoints\n"
        << "  --tick-us N        logic tick interval in microseconds\n"
        << "  --timeout-ms N     scenario timeout\n";
}

Config parseArgs(int argc, char* argv[]) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help" || option == "-h") throw HelpRequested{};
        if (index + 1 >= argc) usageError(std::string(option) + " requires a value");
        const std::string_view value(argv[++index]);
        if (option == "--scenario") {
            config.scenario = value;
        } else if (option == "--messages") {
            config.messages = parseSize(value, option, 1, 10000000);
        } else if (option == "--payload") {
            config.payloadBytes = parseSize(value, option, 1, 16U * 1024U * 1024U);
        } else if (option == "--threads") {
            config.threads = parseSize(value, option, 1, 64);
        } else if (option == "--batch") {
            config.batchSize = parseSize(value, option, 1, 1000000);
        } else if (option == "--fanout") {
            config.fanout = parseSize(value, option, 1, 100000);
        } else if (option == "--tick-us") {
            config.tickInterval =
                std::chrono::microseconds(parseSize(value, option, 1, 1000000));
        } else if (option == "--timeout-ms") {
            config.timeout = std::chrono::milliseconds(parseSize(value, option, 100, 300000));
        } else {
            usageError(std::string("unknown option: ") + std::string(option));
        }
    }
    if (config.scenario != "framing" && config.scenario != "logic-queue" &&
        config.scenario != "broadcast-fanout") {
        usageError("--scenario must be framing, logic-queue, or broadcast-fanout");
    }
    return config;
}

std::size_t checkedMultiply(std::size_t left, std::size_t right, std::string_view label) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::invalid_argument(std::string(label) + " exceeds addressable size");
    }
    return left * right;
}

double elapsedMilliseconds(Clock::time_point start, Clock::time_point finish) {
    return std::chrono::duration<double, std::milli>(finish - start).count();
}

double elapsedSeconds(Clock::time_point start, Clock::time_point finish) {
    return std::chrono::duration<double>(finish - start).count();
}

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) throw std::runtime_error("cannot calculate percentile without samples");
    std::sort(samples.begin(), samples.end());
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * samples.size()));
    return samples[(std::max<std::size_t>)(1, rank) - 1];
}

std::uint64_t sampleWorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(
            ::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) == FALSE) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) {
        throw std::runtime_error("failed to read /proc/self/statm");
    }
    (void)totalPages;
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) throw std::runtime_error("sysconf(_SC_PAGESIZE) failed");
    return residentPages * static_cast<std::uint64_t>(pageSize);
#endif
}

class WorkingSetSampler {
public:
    explicit WorkingSetSampler(std::uint64_t initial) : peak_(initial) {
        thread_ = std::thread([this] {
            while (running_.load(std::memory_order_acquire)) {
                try {
                    observe(sampleWorkingSetBytes());
                } catch (...) {
                    failed_.store(true, std::memory_order_release);
                    running_.store(false, std::memory_order_release);
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
        });
    }

    WorkingSetSampler(const WorkingSetSampler&) = delete;
    WorkingSetSampler& operator=(const WorkingSetSampler&) = delete;

    ~WorkingSetSampler() {
        if (thread_.joinable()) {
            try {
                (void)stop();
            } catch (...) {
            }
        }
    }

    std::uint64_t stop(std::uint64_t finalSample = 0) {
        if (finalSample != 0) observe(finalSample);
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        if (failed_.load(std::memory_order_acquire)) {
            throw std::runtime_error("working-set sampler failed");
        }
        return peak_.load(std::memory_order_acquire);
    }

private:
    void observe(std::uint64_t sample) noexcept {
        auto observed = peak_.load(std::memory_order_relaxed);
        while (sample > observed &&
               !peak_.compare_exchange_weak(
                   observed,
                   sample,
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    std::atomic<bool> running_{true};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> peak_;
    std::thread thread_;
};

Result runFraming(const Config& config) {
    const auto frameBytes = checkedMultiply(1, config.payloadBytes + gamenet::protocol::PacketFramer::kLengthBytes,
                                            "framing frame bytes");
    const auto batchWireBytes = checkedMultiply(frameBytes, config.batchSize, "framing batch bytes");
    gamenet::protocol::PacketFramer framer({
        .maxPayloadBytes = config.payloadBytes,
        .maxBufferedBytes = batchWireBytes,
        .maxFramesPerPush = config.batchSize,
        .maxFrameBytesPerPush = batchWireBytes,
    });

    const std::string payload(config.payloadBytes, 'p');
    const auto encoded = framer.encode(payload);
    if (!encoded) throw std::runtime_error("failed to encode framing benchmark payload");

    std::string fullBatch;
    fullBatch.reserve(batchWireBytes);
    for (std::size_t index = 0; index < config.batchSize; ++index) fullBatch += *encoded;
    const auto tailCount = config.messages % config.batchSize;
    std::string tailBatch;
    tailBatch.reserve(checkedMultiply(frameBytes, tailCount, "framing tail bytes"));
    for (std::size_t index = 0; index < tailCount; ++index) tailBatch += *encoded;

    std::size_t decodedFrames = 0;
    std::size_t decodedPayloadBytes = 0;
    const auto start = Clock::now();
    std::size_t remaining = config.messages;
    while (remaining != 0) {
        const auto count = (std::min)(remaining, config.batchSize);
        auto result = framer.push(count == config.batchSize ? std::string_view(fullBatch)
                                                            : std::string_view(tailBatch));
        for (;;) {
            if (result.status == gamenet::protocol::FrameStatus::FrameTooLarge ||
                result.status == gamenet::protocol::FrameStatus::BufferLimitExceeded ||
                result.status == gamenet::protocol::FrameStatus::Faulted) {
                throw std::runtime_error("PacketFramer faulted during valid benchmark input");
            }
            for (const auto& frame : result.frames) {
                if (frame.size() != config.payloadBytes) {
                    throw std::runtime_error("PacketFramer returned an unexpected payload size");
                }
                ++decodedFrames;
                decodedPayloadBytes += frame.size();
            }
            if (!result.needsContinuation) break;
            result = framer.push({});
        }
        remaining -= count;
    }
    const auto finish = Clock::now();

    if (decodedFrames != config.messages ||
        decodedPayloadBytes != checkedMultiply(config.messages, config.payloadBytes, "decoded bytes") ||
        framer.bufferedBytes() != 0 || framer.faulted()) {
        throw std::runtime_error("framing benchmark did not consume the exact configured input");
    }

    const auto seconds = elapsedSeconds(start, finish);
    if (seconds <= 0.0) throw std::runtime_error("framing benchmark clock did not advance");
    Result result;
    result.elapsedMs = elapsedMilliseconds(start, finish);
    result.throughputMibPerSecond =
        (static_cast<double>(config.messages) * static_cast<double>(frameBytes)) /
        kBytesPerMebibyte / seconds;
    result.operationsPerSecond = static_cast<double>(config.messages) / seconds;
    return result;
}

Result runLogicQueue(const Config& config) {
    const auto queueBytes = checkedMultiply(config.messages, config.payloadBytes, "logic queue bytes");
    gamenet::net::EventLoop loop;
    gamenet::game_logic::LogicLoop logic(
        &loop,
        {.tickInterval = config.tickInterval,
         .maxCommandsPerTick = config.batchSize,
         .queueLimits = {
             .maxCommands = config.messages,
             .maxQueuedBytes = queueBytes,
             .maxPayloadBytes = config.payloadBytes,
         }});

    std::vector<double> lagSamples;
    lagSamples.reserve(config.messages);
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> remainingProducers{config.threads};
    std::atomic<bool> timedOut{false};

    logic.setHandler([&](gamenet::game_logic::GameCommand command)
                         -> std::optional<gamenet::game_logic::GameCommand> {
        const auto lag = std::chrono::duration<double, std::micro>(Clock::now() - command.enqueuedAt);
        lagSamples.push_back(lag.count());
        if (lagSamples.size() == config.messages) loop.quit();
        return std::nullopt;
    });
    logic.start();
    const auto timeoutTimer = loop.runAfter(config.timeout, [&] {
        timedOut.store(true, std::memory_order_release);
        loop.quit();
    });

    const std::string payload(config.payloadBytes, 'q');
    const auto executor = loop.executor();
    const auto start = Clock::now();
    std::mutex producerErrorMutex;
    std::exception_ptr producerError;
    std::vector<std::jthread> producers;
    producers.reserve(config.threads);
    for (std::size_t producer = 0; producer < config.threads; ++producer) {
        producers.emplace_back([&, producer] {
            try {
                for (std::size_t index = producer; index < config.messages; index += config.threads) {
                    gamenet::game_logic::GameCommand command;
                    command.messageId = static_cast<std::uint32_t>(index);
                    command.requestId = index;
                    command.payload = payload;
                    const auto submitResult = logic.submit(std::move(command));
                    if (submitResult == gamenet::game_logic::SubmitResult::Accepted) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        rejected.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                std::lock_guard lock(producerErrorMutex);
                if (!producerError) producerError = std::current_exception();
            }
            const bool lastProducer =
                remainingProducers.fetch_sub(1, std::memory_order_acq_rel) == 1;
            bool failed = rejected.load(std::memory_order_acquire) != 0;
            {
                std::lock_guard lock(producerErrorMutex);
                failed = failed || producerError != nullptr;
            }
            if (lastProducer && failed) {
                (void)executor.tryQueue([&loop] { loop.quit(); });
            }
        });
    }

    loop.loop();
    const auto finish = Clock::now();
    for (auto& producer : producers) producer.join();
    {
        std::lock_guard lock(producerErrorMutex);
        if (producerError) std::rethrow_exception(producerError);
    }
    loop.cancel(timeoutTimer);
    const auto snapshotBeforeStop = logic.queueSnapshot();
    (void)logic.stop();

    if (timedOut.load(std::memory_order_acquire)) {
        throw std::runtime_error("logic-queue benchmark timed out");
    }
    if (accepted.load(std::memory_order_acquire) != config.messages ||
        rejected.load(std::memory_order_acquire) != 0 || lagSamples.size() != config.messages ||
        snapshotBeforeStop.depth != 0) {
        throw std::runtime_error("logic-queue benchmark did not accept and handle every command");
    }

    Result result;
    result.elapsedMs = elapsedMilliseconds(start, finish);
    result.operationsPerSecond =
        static_cast<double>(config.messages) / elapsedSeconds(start, finish);
    result.logicQueueLagP50Us = percentile(lagSamples, 0.50);
    result.logicQueueLagP99Us = percentile(std::move(lagSamples), 0.99);
    result.logicDepthHighWatermark = snapshotBeforeStop.depthHighWatermark;
    result.logicBytesHighWatermark = snapshotBeforeStop.bytesHighWatermark;
    result.logicAccepted = snapshotBeforeStop.accepted;
    result.logicRejected = snapshotBeforeStop.rejectedFull + snapshotBeforeStop.rejectedPayload +
                           snapshotBeforeStop.rejectedStopped;
    return result;
}

struct FanoutProbe {
    std::mutex mutex;
    std::condition_variable condition;
    Clock::time_point iterationStart{};
    Clock::time_point dispatchStart{};
    std::size_t expected{};
    std::size_t completed{};
    std::vector<double> endpointLatencyUs;
    std::vector<double> queueLatencyUs;

    void begin(std::size_t expectedEndpoints, Clock::time_point start) {
        std::lock_guard lock(mutex);
        iterationStart = start;
        expected = expectedEndpoints;
        completed = 0;
    }

    void markDispatchStart(Clock::time_point start) {
        std::lock_guard lock(mutex);
        dispatchStart = start;
    }

    void record(Clock::time_point now) {
        std::lock_guard lock(mutex);
        endpointLatencyUs.push_back(
            std::chrono::duration<double, std::micro>(now - iterationStart).count());
        queueLatencyUs.push_back(
            std::chrono::duration<double, std::micro>(now - dispatchStart).count());
        ++completed;
        if (completed == expected) condition.notify_one();
    }

    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [&] { return completed == expected; });
    }
};

class BenchmarkEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    BenchmarkEndpoint(
        std::uint64_t id,
        gamenet::net::EventLoopExecutor executor,
        FanoutProbe* probe)
        : id_{id}, executor_(std::move(executor)), probe_(probe) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override { return executor_; }

    gamenet::transport::EndpointResult send(std::string_view bytes) override {
        if (!executor_.isInOwnerThread()) return gamenet::transport::EndpointResult::WrongThread;
        if (!open_.load(std::memory_order_acquire)) return gamenet::transport::EndpointResult::Closed;
        if (bytes.empty()) return gamenet::transport::EndpointResult::Closed;
        probe_->record(Clock::now());
        return gamenet::transport::EndpointResult::Accepted;
    }

    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_.store(false, std::memory_order_release);
        return gamenet::transport::EndpointResult::Accepted;
    }

    bool isOpen() const noexcept override { return open_.load(std::memory_order_acquire); }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoopExecutor executor_;
    FanoutProbe* probe_;
    std::atomic<bool> open_{true};
};

Result runBroadcastFanout(const Config& config) {
    const auto hardBytes = checkedMultiply(config.fanout, config.payloadBytes, "broadcast hard bytes");
    const auto taskBytes = checkedMultiply(config.batchSize, config.payloadBytes, "broadcast task bytes");
    const auto before = sampleWorkingSetBytes();
    WorkingSetSampler memorySampler(before);
    gamenet::net::EventLoop managementLoop;
    FanoutProbe probe;

    // FanoutProbe must outlive owner-loop tasks, including timeout/error teardown.
    // This declaration order makes ownerThreads stop/join before probe destruction.
    std::vector<std::unique_ptr<gamenet::net::EventLoopThread>> ownerThreads;
    std::vector<gamenet::net::EventLoop*> ownerLoops;
    ownerThreads.reserve(config.threads);
    ownerLoops.reserve(config.threads);
    for (std::size_t index = 0; index < config.threads; ++index) {
        auto thread = std::make_unique<gamenet::net::EventLoopThread>();
        ownerLoops.push_back(thread->startLoop());
        ownerThreads.push_back(std::move(thread));
    }

    probe.endpointLatencyUs.reserve(checkedMultiply(config.fanout, config.messages, "latency samples"));
    probe.queueLatencyUs.reserve(checkedMultiply(config.fanout, config.messages, "queue samples"));
    std::vector<std::shared_ptr<BenchmarkEndpoint>> endpoints;
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> sessions;
    endpoints.reserve(config.fanout);
    sessions.reserve(config.fanout);
    for (std::size_t index = 0; index < config.fanout; ++index) {
        auto endpoint = std::make_shared<BenchmarkEndpoint>(
            index + 1,
            ownerLoops[index % ownerLoops.size()]->executor(),
            &probe);
        auto session = std::make_shared<gamenet::game_session::PlayerSession>(
            index + 1,
            "benchmark-player-" + std::to_string(index),
            endpoint,
            gamenet::game_session::PlayerSession::Clock::now());
        session->markOnline(gamenet::game_session::PlayerSession::Clock::now());
        endpoints.push_back(std::move(endpoint));
        sessions.push_back(std::move(session));
    }

    gamenet::broadcast::BroadcastRouter router(
        &managementLoop,
        {.softFanout = config.fanout,
         .hardFanout = config.fanout,
         .softBytes = hardBytes,
         .hardBytes = hardBytes});
    gamenet::broadcast::BroadcastDispatcher dispatcher(
        {.maxEndpointsPerTask = config.batchSize, .maxBytesPerTask = taskBytes});

    std::vector<double> completionLatencyUs;
    completionLatencyUs.reserve(config.messages);
    std::vector<double> routeLatencyUs;
    routeLatencyUs.reserve(config.messages);
    std::size_t accepted = 0;
    std::size_t dropped = 0;
    std::size_t scheduledTasks = 0;
    std::size_t taskHighWatermark = 0;
    const std::string payloadBytes(config.payloadBytes, 'b');
    const auto start = Clock::now();
    for (std::size_t iteration = 0; iteration < config.messages; ++iteration) {
        auto payload = std::make_shared<const std::string>(payloadBytes);
        const auto iterationStart = Clock::now();
        probe.begin(config.fanout, iterationStart);
        auto plan = router.route(payload, sessions);
        const auto routeFinished = Clock::now();
        routeLatencyUs.push_back(
            std::chrono::duration<double, std::micro>(routeFinished - iterationStart).count());
        accepted += plan.accepted();
        dropped += plan.dropped();
        if (plan.accepted() != config.fanout || plan.dropped() != 0) {
            throw std::runtime_error("broadcast router did not accept the configured fanout");
        }
        probe.markDispatchStart(routeFinished);
        const auto summary = dispatcher.dispatch(std::move(plan));
        if (summary.scheduledEndpoints != config.fanout || summary.scheduledTasks == 0) {
            throw std::runtime_error("broadcast dispatcher did not schedule the configured fanout");
        }
        scheduledTasks += summary.scheduledTasks;
        taskHighWatermark = (std::max)(taskHighWatermark, summary.scheduledTasks);
        if (!probe.wait(config.timeout)) {
            throw std::runtime_error("broadcast-fanout benchmark timed out");
        }
        completionLatencyUs.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - iterationStart).count());
    }
    const auto finish = Clock::now();
    const auto after = sampleWorkingSetBytes();
    const auto peak = memorySampler.stop(after);

    if (probe.endpointLatencyUs.size() !=
        checkedMultiply(config.fanout, config.messages, "completed endpoint sends")) {
        throw std::runtime_error("broadcast benchmark did not observe every endpoint send");
    }

    sessions.clear();
    endpoints.clear();
    for (auto& thread : ownerThreads) thread->stop();

    Result result;
    result.elapsedMs = elapsedMilliseconds(start, finish);
    result.operationsPerSecond =
        static_cast<double>(config.fanout) * static_cast<double>(config.messages) /
        elapsedSeconds(start, finish);
    result.broadcastEndpointLatencyP50Us = percentile(probe.endpointLatencyUs, 0.50);
    result.broadcastEndpointLatencyP99Us = percentile(std::move(probe.endpointLatencyUs), 0.99);
    result.broadcastRouteLatencyP50Us = percentile(routeLatencyUs, 0.50);
    result.broadcastRouteLatencyP99Us = percentile(std::move(routeLatencyUs), 0.99);
    result.broadcastQueueLatencyP50Us = percentile(probe.queueLatencyUs, 0.50);
    result.broadcastQueueLatencyP99Us = percentile(std::move(probe.queueLatencyUs), 0.99);
    result.broadcastFanoutCompletionP50Us = percentile(completionLatencyUs, 0.50);
    result.broadcastFanoutCompletionP99Us = percentile(std::move(completionLatencyUs), 0.99);
    result.broadcastAccepted = accepted;
    result.broadcastDropped = dropped;
    result.broadcastScheduledTasks = scheduledTasks;
    result.broadcastTaskHighWatermark = taskHighWatermark;
    result.workingSetBeforeBytes = before;
    result.workingSetAfterBytes = after;
    result.workingSetPeakBytes = peak;
    result.workingSetPeakDeltaBytes = static_cast<std::int64_t>(peak - before);
    return result;
}

std::string jsonEscape(std::string_view value) {
    std::string output;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20U) {
                    constexpr char digits[] = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(digits[ch >> 4U]);
                    output.push_back(digits[ch & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
        }
    }
    return output;
}

template <typename Value>
void printOptional(std::ostream& output, const std::optional<Value>& value) {
    if (value) {
        output << *value;
    } else {
        output << "null";
    }
}

std::string_view platformName() noexcept {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

std::string_view backendName() noexcept {
#ifdef _WIN32
    return "iocp";
#else
    return "epoll";
#endif
}

void printDocument(
    const Config& config,
    const Result& result,
    std::string_view status,
    std::string_view error = {}) {
    std::cout << std::fixed << std::setprecision(3)
              << "{\n"
              << "  \"schema\": \"gamenet.phase4_benchmark.v1\",\n"
              << "  \"status\": \"" << status << "\",\n"
              << "  \"error\": ";
    if (error.empty()) {
        std::cout << "null";
    } else {
        std::cout << '"' << jsonEscape(error) << '"';
    }
    std::cout << ",\n"
              << "  \"scenario\": \"" << jsonEscape(config.scenario) << "\",\n"
              << "  \"platform\": \"" << platformName() << "\",\n"
              << "  \"backend\": \"" << backendName() << "\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"parameters\": {\n"
              << "    \"messages\": " << config.messages << ",\n"
              << "    \"payload_bytes\": " << config.payloadBytes << ",\n"
              << "    \"threads\": " << config.threads << ",\n"
              << "    \"batch_size\": " << config.batchSize << ",\n"
              << "    \"fanout\": " << config.fanout << ",\n"
              << "    \"tick_interval_us\": " << config.tickInterval.count() << ",\n"
              << "    \"timeout_ms\": " << config.timeout.count() << "\n"
              << "  },\n"
              << "  \"measurements\": {\n"
              << "    \"elapsed_ms\": " << result.elapsedMs << ",\n"
              << "    \"throughput_mib_per_second\": ";
    printOptional(std::cout, result.throughputMibPerSecond);
    std::cout << ",\n    \"operations_per_second\": ";
    printOptional(std::cout, result.operationsPerSecond);
    std::cout << ",\n    \"logic_queue_lag_p50_us\": ";
    printOptional(std::cout, result.logicQueueLagP50Us);
    std::cout << ",\n    \"logic_queue_lag_p99_us\": ";
    printOptional(std::cout, result.logicQueueLagP99Us);
    std::cout << ",\n    \"logic_depth_high_watermark\": ";
    printOptional(std::cout, result.logicDepthHighWatermark);
    std::cout << ",\n    \"logic_bytes_high_watermark\": ";
    printOptional(std::cout, result.logicBytesHighWatermark);
    std::cout << ",\n    \"logic_accepted\": ";
    printOptional(std::cout, result.logicAccepted);
    std::cout << ",\n    \"logic_rejected\": ";
    printOptional(std::cout, result.logicRejected);
    std::cout << ",\n    \"broadcast_endpoint_latency_p50_us\": ";
    printOptional(std::cout, result.broadcastEndpointLatencyP50Us);
    std::cout << ",\n    \"broadcast_endpoint_latency_p99_us\": ";
    printOptional(std::cout, result.broadcastEndpointLatencyP99Us);
    std::cout << ",\n    \"broadcast_route_latency_p50_us\": ";
    printOptional(std::cout, result.broadcastRouteLatencyP50Us);
    std::cout << ",\n    \"broadcast_route_latency_p99_us\": ";
    printOptional(std::cout, result.broadcastRouteLatencyP99Us);
    std::cout << ",\n    \"broadcast_queue_latency_p50_us\": ";
    printOptional(std::cout, result.broadcastQueueLatencyP50Us);
    std::cout << ",\n    \"broadcast_queue_latency_p99_us\": ";
    printOptional(std::cout, result.broadcastQueueLatencyP99Us);
    std::cout << ",\n    \"broadcast_fanout_completion_p50_us\": ";
    printOptional(std::cout, result.broadcastFanoutCompletionP50Us);
    std::cout << ",\n    \"broadcast_fanout_completion_p99_us\": ";
    printOptional(std::cout, result.broadcastFanoutCompletionP99Us);
    std::cout << ",\n    \"broadcast_accepted\": ";
    printOptional(std::cout, result.broadcastAccepted);
    std::cout << ",\n    \"broadcast_dropped\": ";
    printOptional(std::cout, result.broadcastDropped);
    std::cout << ",\n    \"broadcast_scheduled_tasks\": ";
    printOptional(std::cout, result.broadcastScheduledTasks);
    std::cout << ",\n    \"broadcast_task_high_watermark\": ";
    printOptional(std::cout, result.broadcastTaskHighWatermark);
    std::cout << ",\n    \"working_set_before_bytes\": ";
    printOptional(std::cout, result.workingSetBeforeBytes);
    std::cout << ",\n    \"working_set_after_bytes\": ";
    printOptional(std::cout, result.workingSetAfterBytes);
    std::cout << ",\n    \"working_set_peak_bytes\": ";
    printOptional(std::cout, result.workingSetPeakBytes);
    std::cout << ",\n    \"working_set_peak_delta_bytes\": ";
    printOptional(std::cout, result.workingSetPeakDeltaBytes);
    std::cout << "\n  }\n}\n";
}

Result run(const Config& config) {
    if (config.scenario == "framing") return runFraming(config);
    if (config.scenario == "logic-queue") return runLogicQueue(config);
    return runBroadcastFanout(config);
}

}  // namespace

int main(int argc, char* argv[]) {
    Config config;
    try {
        config = parseArgs(argc, argv);
        printDocument(config, run(config), "ok");
        return 0;
    } catch (const HelpRequested&) {
        printUsage(std::cout);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gamenet_phase4_benchmark: " << error.what() << '\n';
        printDocument(config, {}, "error", error.what());
        return 1;
    }
}
