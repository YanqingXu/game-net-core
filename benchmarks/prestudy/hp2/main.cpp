// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerView.h"
#include "hp2/MailboxSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using OwnedPacket = gamenet::experimental::hp1::OwnedPacket;
using Command = gamenet::experimental::hp2::DataPlaneCommand<OwnedPacket>;
using gamenet::experimental::hp2::MailboxPushStatus;
using gamenet::experimental::hp2::SpscMailboxSource;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

struct Options {
    std::size_t messages{200'000};
    std::size_t payloadBytes{256};
    std::size_t capacity{4'096};
    std::size_t batch{64};
};

std::size_t parsePositive(std::string_view value, std::string_view option) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(std::string(value), &consumed);
    if (consumed != value.size() || parsed == 0 ||
        parsed > (std::numeric_limits<std::size_t>::max)()) {
        throw std::invalid_argument(std::string(option) + " requires a positive size");
    }
    return static_cast<std::size_t>(parsed);
}

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto requireValue = [&](std::string_view name) {
            if (++index >= argc) throw std::invalid_argument(std::string(name) + " requires a value");
            return std::string_view(argv[index]);
        };
        if (argument == "--messages") {
            options.messages = parsePositive(requireValue(argument), argument);
        } else if (argument == "--payload-bytes") {
            options.payloadBytes = parsePositive(requireValue(argument), argument);
        } else if (argument == "--capacity") {
            options.capacity = parsePositive(requireValue(argument), argument);
        } else if (argument == "--batch") {
            options.batch = parsePositive(requireValue(argument), argument);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

template <typename T>
struct CountingAllocator {
    using value_type = T;

    CountingAllocator() = default;
    explicit CountingAllocator(std::atomic<std::uint64_t>* counter) noexcept
        : counter(counter) {}

    template <typename U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept
        : counter(other.counter) {}

    T* allocate(std::size_t count) {
        counter->fetch_add(1, std::memory_order_relaxed);
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }

    template <typename U>
    bool operator==(const CountingAllocator<U>& other) const noexcept {
        return counter == other.counter;
    }

    template <typename U>
    bool operator!=(const CountingAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

    std::atomic<std::uint64_t>* counter{};

    template <typename>
    friend struct CountingAllocator;
};

class MutexMailbox {
public:
    MutexMailbox(std::size_t capacity, bool coalesceNotifications)
        : capacity_(capacity),
          coalesceNotifications_(coalesceNotifications),
          queue_(CountingAllocator<Command>(&allocatorCalls_)) {}

    MailboxPushStatus tryPush(Command&& value) {
        std::lock_guard lock(mutex_);
        ++lockAcquisitions_;
        if (!accepting_) return MailboxPushStatus::Stopped;
        if (queue_.size() == capacity_) {
            ++rejectedFull_;
            return MailboxPushStatus::QueueFull;
        }
        queue_.emplace_back(std::move(value));
        ++accepted_;
        highWatermark_ = (std::max)(highWatermark_, queue_.size());
        if (!coalesceNotifications_ || !notificationPending_) {
            notificationPending_ = true;
            ++physicalNotifications_;
        } else {
            ++mergedNotifications_;
        }
        return MailboxPushStatus::Accepted;
    }

    template <typename Visitor>
    std::size_t drain(std::size_t maxItems, Visitor&& visitor) {
        std::vector<Command> local;
        local.reserve(maxItems);
        {
            std::lock_guard lock(mutex_);
            ++lockAcquisitions_;
            const auto count = (std::min)(maxItems, queue_.size());
            for (std::size_t index = 0; index < count; ++index) {
                local.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            if (queue_.empty()) {
                notificationPending_ = false;
            } else {
                ++continuations_;
            }
        }
        for (auto& value : local) {
            visitor(std::move(value));
            ++processed_;
        }
        return local.size();
    }

    bool empty() const {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

    void stop() {
        std::lock_guard lock(mutex_);
        accepting_ = false;
    }

    std::uint64_t allocatorCalls() const noexcept {
        return allocatorCalls_.load(std::memory_order_relaxed);
    }
    std::uint64_t lockAcquisitions() const noexcept {
        return lockAcquisitions_.load(std::memory_order_relaxed);
    }
    std::uint64_t physicalNotifications() const noexcept {
        return physicalNotifications_.load(std::memory_order_relaxed);
    }
    std::uint64_t mergedNotifications() const noexcept {
        return mergedNotifications_.load(std::memory_order_relaxed);
    }
    std::uint64_t continuations() const noexcept {
        return continuations_.load(std::memory_order_relaxed);
    }
    std::uint64_t accepted() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }
    std::uint64_t processed() const noexcept {
        return processed_.load(std::memory_order_relaxed);
    }
    std::uint64_t rejectedFull() const noexcept {
        return rejectedFull_.load(std::memory_order_relaxed);
    }
    std::size_t highWatermark() const noexcept { return highWatermark_; }

private:
    using Queue = std::deque<Command, CountingAllocator<Command>>;

    const std::size_t capacity_;
    const bool coalesceNotifications_;
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> allocatorCalls_{};
    Queue queue_;
    bool accepting_{true};
    bool notificationPending_{false};
    std::size_t highWatermark_{};
    std::atomic<std::uint64_t> lockAcquisitions_{};
    std::atomic<std::uint64_t> physicalNotifications_{};
    std::atomic<std::uint64_t> mergedNotifications_{};
    std::atomic<std::uint64_t> continuations_{};
    std::atomic<std::uint64_t> accepted_{};
    std::atomic<std::uint64_t> processed_{};
    std::atomic<std::uint64_t> rejectedFull_{};
};

struct RunMetrics {
    std::uint64_t elapsedNs{};
    std::uint64_t checksum{};
    std::uint64_t queueAgeSumNs{};
    std::uint64_t queueAgeMaxNs{};
    std::uint64_t queueAllocatorCalls{};
    std::uint64_t lockAcquisitions{};
    std::uint64_t physicalNotifications{};
    std::uint64_t mergedNotifications{};
    std::uint64_t ownerContinuations{};
    std::uint64_t genericPosts{};
    std::uint64_t fullRetries{};
    std::size_t highWatermark{};
    bool settled{};
};

struct AgeMetrics {
    std::uint64_t sumNs{};
    std::uint64_t maxNs{};
};

std::string payloadSeed(std::size_t bytes) {
    std::string payload(bytes, '\0');
    for (std::size_t index = 0; index < bytes; ++index) {
        payload[index] = static_cast<char>('a' + (index % 23));
    }
    return payload;
}

void accountAge(AgeMetrics& metrics, std::uint64_t enqueuedAtNs) {
    const auto age = nowNs() - enqueuedAtNs;
    metrics.sumNs += age;
    metrics.maxNs = (std::max)(metrics.maxNs, age);
}

RunMetrics runMutexBaseline(const Options& options, const std::string& seed) {
    MutexMailbox inbound(options.capacity, true);
    MutexMailbox outbox(options.capacity, false);
    RunMetrics metrics;
    AgeMetrics inboundAge;
    AgeMetrics outboxAge;
    std::atomic<bool> networkDone{false};
    const auto started = Clock::now();

    std::thread logic([&] {
        std::size_t handled = 0;
        while (handled != options.messages) {
            const auto count = inbound.drain(options.batch, [&](Command value) {
                accountAge(inboundAge, value.enqueuedAtNs);
                Command output(
                    value.routeId, value.routeGeneration, nowNs(),
                    std::move(value.packet));
                while (outbox.tryPush(std::move(output)) == MailboxPushStatus::QueueFull) {
                    std::this_thread::yield();
                }
                ++handled;
            });
            if (count == 0) std::this_thread::yield();
        }
    });

    std::size_t responses = 0;
    for (std::size_t index = 0; index < options.messages; ++index) {
        Command input(1, 1, nowNs(), OwnedPacket(std::string(seed)));
        while (inbound.tryPush(std::move(input)) == MailboxPushStatus::QueueFull) {
            (void)outbox.drain(options.batch, [&](Command value) {
                accountAge(outboxAge, value.enqueuedAtNs);
                metrics.checksum += static_cast<unsigned char>(value.packet.asStringView().front());
                ++responses;
            });
            std::this_thread::yield();
        }
        (void)outbox.drain(options.batch, [&](Command value) {
            accountAge(outboxAge, value.enqueuedAtNs);
            metrics.checksum += static_cast<unsigned char>(value.packet.asStringView().front());
            ++responses;
        });
    }
    networkDone.store(true, std::memory_order_release);
    while (responses != options.messages) {
        if (outbox.drain(options.batch, [&](Command value) {
                accountAge(outboxAge, value.enqueuedAtNs);
                metrics.checksum += static_cast<unsigned char>(value.packet.asStringView().front());
                ++responses;
            }) == 0) {
            std::this_thread::yield();
        }
    }
    logic.join();
    inbound.stop();
    outbox.stop();
    metrics.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    metrics.queueAgeSumNs = inboundAge.sumNs + outboxAge.sumNs;
    metrics.queueAgeMaxNs = (std::max)(inboundAge.maxNs, outboxAge.maxNs);
    metrics.queueAllocatorCalls = inbound.allocatorCalls() + outbox.allocatorCalls();
    metrics.lockAcquisitions = inbound.lockAcquisitions() + outbox.lockAcquisitions();
    metrics.physicalNotifications =
        inbound.physicalNotifications() + outbox.physicalNotifications();
    metrics.mergedNotifications =
        inbound.mergedNotifications() + outbox.mergedNotifications();
    metrics.ownerContinuations = inbound.continuations() + outbox.continuations();
    metrics.genericPosts = outbox.accepted();
    metrics.fullRetries = inbound.rejectedFull() + outbox.rejectedFull();
    metrics.highWatermark = (std::max)(inbound.highWatermark(), outbox.highWatermark());
    metrics.settled = networkDone.load(std::memory_order_acquire) && inbound.empty() &&
        outbox.empty() && inbound.accepted() == options.messages &&
        inbound.processed() == options.messages && outbox.accepted() == options.messages &&
        outbox.processed() == options.messages;
    return metrics;
}

RunMetrics runSpscCandidate(const Options& options, const std::string& seed) {
    SpscMailboxSource<Command> inbound(options.capacity, [] {});
    SpscMailboxSource<Command> outbox(options.capacity, [] {});
    auto networkProducer = inbound.producerHandle();
    auto logicProducer = outbox.producerHandle();
    RunMetrics metrics;
    AgeMetrics inboundAge;
    AgeMetrics outboxAge;
    const auto started = Clock::now();

    std::thread logic([&] {
        std::size_t handled = 0;
        while (handled != options.messages) {
            const auto result = inbound.drain(options.batch, [&](Command value) {
                accountAge(inboundAge, value.enqueuedAtNs);
                Command output(
                    value.routeId, value.routeGeneration, nowNs(),
                    std::move(value.packet));
                while (logicProducer.tryPush(std::move(output)).status ==
                       MailboxPushStatus::QueueFull) {
                    std::this_thread::yield();
                }
                ++handled;
            });
            if (result.processed == 0) std::this_thread::yield();
        }
    });

    std::size_t responses = 0;
    for (std::size_t index = 0; index < options.messages; ++index) {
        Command input(1, 1, nowNs(), OwnedPacket(std::string(seed)));
        while (networkProducer.tryPush(std::move(input)).status ==
               MailboxPushStatus::QueueFull) {
            (void)outbox.drain(options.batch, [&](Command value) {
                accountAge(outboxAge, value.enqueuedAtNs);
                metrics.checksum += static_cast<unsigned char>(value.packet.asStringView().front());
                ++responses;
            });
            std::this_thread::yield();
        }
        (void)outbox.drain(options.batch, [&](Command value) {
            accountAge(outboxAge, value.enqueuedAtNs);
            metrics.checksum += static_cast<unsigned char>(value.packet.asStringView().front());
            ++responses;
        });
    }
    while (responses != options.messages) {
        const auto result = outbox.drain(options.batch, [&](Command value) {
            accountAge(outboxAge, value.enqueuedAtNs);
            metrics.checksum += static_cast<unsigned char>(value.packet.asStringView().front());
            ++responses;
        });
        if (result.processed == 0) std::this_thread::yield();
    }
    logic.join();
    inbound.beginStop();
    outbox.beginStop();
    const auto inboundSnapshot = inbound.snapshot();
    const auto outboxSnapshot = outbox.snapshot();
    metrics.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    metrics.queueAgeSumNs = inboundAge.sumNs + outboxAge.sumNs;
    metrics.queueAgeMaxNs = (std::max)(inboundAge.maxNs, outboxAge.maxNs);
    metrics.queueAllocatorCalls = 0;
    metrics.lockAcquisitions = 0;
    metrics.physicalNotifications = inboundSnapshot.physicalNotifications +
        outboxSnapshot.physicalNotifications;
    metrics.mergedNotifications = inboundSnapshot.mergedNotifications +
        outboxSnapshot.mergedNotifications;
    metrics.ownerContinuations = inboundSnapshot.ownerContinuations +
        outboxSnapshot.ownerContinuations;
    metrics.genericPosts = 0;
    metrics.fullRetries = inboundSnapshot.mailbox.rejectedFull +
        outboxSnapshot.mailbox.rejectedFull;
    metrics.highWatermark = (std::max)(
        inboundSnapshot.mailbox.depthHighWatermark,
        outboxSnapshot.mailbox.depthHighWatermark);
    metrics.settled = inboundSnapshot.settled() && outboxSnapshot.settled();
    return metrics;
}

void writeMetrics(std::string_view name, const RunMetrics& metrics, const Options& options) {
    const auto throughput = metrics.elapsedNs == 0
        ? 0.0
        : static_cast<double>(options.messages) * 1'000'000'000.0 /
            static_cast<double>(metrics.elapsedNs);
    const auto meanAge = options.messages == 0
        ? 0.0
        : static_cast<double>(metrics.queueAgeSumNs) /
            static_cast<double>(options.messages * 2);
    std::cout << "    \"" << name << "\": {\n"
              << "      \"elapsed_ns\": " << metrics.elapsedNs << ",\n"
              << "      \"messages_per_second\": " << throughput << ",\n"
              << "      \"queue_age_mean_ns\": " << meanAge << ",\n"
              << "      \"queue_age_max_ns\": " << metrics.queueAgeMaxNs << ",\n"
              << "      \"queue_allocator_calls\": " << metrics.queueAllocatorCalls << ",\n"
              << "      \"lock_acquisitions\": " << metrics.lockAcquisitions << ",\n"
              << "      \"physical_notifications\": " << metrics.physicalNotifications << ",\n"
              << "      \"merged_notifications\": " << metrics.mergedNotifications << ",\n"
              << "      \"owner_continuations\": " << metrics.ownerContinuations << ",\n"
              << "      \"generic_posts\": " << metrics.genericPosts << ",\n"
              << "      \"full_retries\": " << metrics.fullRetries << ",\n"
              << "      \"depth_high_watermark\": " << metrics.highWatermark << ",\n"
              << "      \"checksum\": " << metrics.checksum << ",\n"
              << "      \"shutdown_residue\": " << (metrics.settled ? 0 : 1) << "\n"
              << "    }";
}

int run(int argc, char* argv[]) {
    const auto options = parseOptions(argc, argv);
    const auto seed = payloadSeed(options.payloadBytes);

    // One unrecorded warmup keeps construction and scheduler startup out of the
    // development observation. It is not HP0 fixed-lab evidence.
    Options warmup = options;
    warmup.messages = (std::min)(options.messages, std::size_t{10'000});
    const auto warmupBaseline = runMutexBaseline(warmup, seed);
    const auto warmupCandidate = runSpscCandidate(warmup, seed);
    if (!warmupBaseline.settled || !warmupCandidate.settled ||
        warmupBaseline.checksum != warmupCandidate.checksum) {
        throw std::runtime_error("HP2 warmup invariant failed");
    }

    const auto baseline = runMutexBaseline(options, seed);
    const auto candidate = runSpscCandidate(options, seed);
    const bool valid = baseline.settled && candidate.settled &&
        baseline.checksum == candidate.checksum &&
        candidate.queueAllocatorCalls == 0 && candidate.lockAcquisitions == 0 &&
        candidate.genericPosts == 0;

    std::cout << "{\n"
              << "  \"schema\": \"gamenet.hp2_spsc_mailbox_prestudy.v1\",\n"
              << "  \"evidence_class\": \"development-only\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"messages\": " << options.messages << ",\n"
              << "  \"payload_bytes\": " << options.payloadBytes << ",\n"
              << "  \"capacity\": " << options.capacity << ",\n"
              << "  \"batch\": " << options.batch << ",\n"
              << "  \"payload_copies_per_message\": 1,\n"
              << "  \"runs\": {\n";
    writeMetrics("callback_mutex", baseline, options);
    std::cout << ",\n";
    writeMetrics("callback_spsc", candidate, options);
    std::cout << "\n  },\n"
              << "  \"valid\": " << (valid ? "true" : "false") << "\n"
              << "}\n";
    return valid ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "gamenet_hp2_spsc_mailbox_benchmark: " << error.what() << '\n';
        return 2;
    }
}
