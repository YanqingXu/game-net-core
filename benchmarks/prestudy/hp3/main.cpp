// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp3/EpollSlotDispatch.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace gamenet::experimental::hp3;

struct Options {
    std::size_t activeSources{1'024};
    std::size_t iterations{200};
    std::size_t duplicateFactor{4};
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
        const auto value = [&]() {
            if (++index >= argc) throw std::invalid_argument(std::string(argument) + " requires a value");
            return std::string_view(argv[index]);
        };
        if (argument == "--active-sources") {
            options.activeSources = parsePositive(value(), argument);
        } else if (argument == "--iterations") {
            options.iterations = parsePositive(value(), argument);
        } else if (argument == "--duplicate-factor") {
            options.duplicateFactor = parsePositive(value(), argument);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    if (options.activeSources > 4'096 ||
        options.duplicateFactor > 4'096 / options.activeSources) {
        throw std::invalid_argument("native event batch must not exceed 4096");
    }
    return options;
}

struct Target {
    std::uint64_t id{};
};

struct BaselineRegistration {
    std::uint64_t token{};
    Target* target{};
    std::uint32_t interests{};
};

struct BaselineNotice {
    std::uint64_t token{};
    Target* target{};
    std::uint32_t events{};
};

struct DecodeRun {
    std::uint64_t elapsedNs{};
    std::uint64_t checksum{};
    std::uint64_t nativeEvents{};
    std::uint64_t deliveredNotices{};
    std::uint64_t mapLookups{};
    std::uint64_t mergeProbes{};
    std::uint64_t slotProbes{};
    std::uint64_t staleEvents{};
    std::uint64_t filteredEvents{};
    bool settled{};
};

std::uint64_t baselineToken(int source, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32U) |
        static_cast<std::uint32_t>(source);
}

DecodeRun runBaseline(
    const std::vector<Target>& targets,
    const std::vector<NativeReadinessEvent>& native,
    std::size_t iterations) {
    std::unordered_map<int, BaselineRegistration> registrations;
    registrations.reserve(targets.size());
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto source = static_cast<int>(1'000 + index);
        const auto token = baselineToken(source, static_cast<std::uint32_t>(index + 1));
        registrations.emplace(source, BaselineRegistration{
            .token = token,
            .target = const_cast<Target*>(&targets[index]),
            .interests = kReadEvent | kWriteEvent,
        });
    }

    std::vector<BaselineNotice> notices;
    notices.reserve(targets.size());
    DecodeRun run;
    const auto started = Clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        notices.clear();
        for (const auto& event : native) {
            ++run.nativeEvents;
            const auto source = static_cast<int>(static_cast<std::uint32_t>(event.token));
            ++run.mapLookups;
            const auto registration = registrations.find(source);
            if (registration == registrations.end() ||
                registration->second.token != event.token) {
                ++run.staleEvents;
                continue;
            }
            const auto events = event.events &
                (registration->second.interests | kErrorEvent | kCloseEvent);
            if (events == 0) {
                ++run.filteredEvents;
                continue;
            }
            bool merged = false;
            for (auto& notice : notices) {
                ++run.mergeProbes;
                if (notice.token == event.token) {
                    notice.events |= events;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                notices.push_back({event.token, registration->second.target, events});
            }
        }
        run.deliveredNotices += notices.size();
        for (const auto& notice : notices) {
            run.checksum += notice.target->id * 17U + notice.events;
        }
    }
    run.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    run.settled = notices.size() == targets.size();
    return run;
}

DecodeRun runCandidate(
    std::vector<Target>& targets,
    const std::vector<NativeReadinessEvent>& native,
    std::size_t iterations) {
    EpollSlotDispatchPrototype arena({
        .capacity = targets.size(),
        .maxNoticesPerBatch = targets.size(),
    });
    std::vector<SlotIdentity> identities;
    identities.reserve(targets.size());
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto source = static_cast<int>(1'000 + index);
        const auto result = arena.registerOrUpdate(
            source, &targets[index], kReadEvent | kWriteEvent);
        if (result.result != SlotDispatchResult::Accepted) {
            throw std::runtime_error("candidate registration failed");
        }
        identities.push_back(result.identity);
    }

    DecodeRun run;
    const auto started = Clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto batch = arena.decode(native);
        run.nativeEvents += batch.metrics.nativeEvents;
        run.deliveredNotices += batch.metrics.deliveredNotices;
        run.mapLookups += batch.metrics.waitHashLookups;
        run.mergeProbes += batch.metrics.mergeProbes;
        run.slotProbes += batch.metrics.slotProbes;
        run.staleEvents += batch.metrics.staleEvents + batch.metrics.malformedEvents;
        run.filteredEvents += batch.metrics.filteredEvents;
        for (const auto& notice : batch.notices) {
            const auto* target = static_cast<const Target*>(notice.target);
            run.checksum += target->id * 17U + notice.events;
        }
        arena.finishBatch();
    }
    run.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());

    for (std::size_t index = 0; index < identities.size(); ++index) {
        if (arena.cancel(identities[index], &targets[index]) != SlotDispatchResult::Accepted) {
            throw std::runtime_error("candidate cancellation failed");
        }
    }
    arena.beginStop();
    run.settled = arena.tryShutdown() && arena.snapshot().settled();
    return run;
}

struct ScenarioResult {
    DecodeRun baseline;
    DecodeRun candidate;
    bool valid{};
};

ScenarioResult runScenario(const Options& options, std::size_t duplicateFactor) {
    std::vector<Target> targets(options.activeSources);
    for (std::size_t index = 0; index < targets.size(); ++index) {
        targets[index].id = index + 1;
    }

    EpollSlotDispatchPrototype tokenSource({
        .capacity = targets.size(),
        .maxNoticesPerBatch = targets.size(),
    });
    std::vector<SlotIdentity> candidateIdentities;
    candidateIdentities.reserve(targets.size());
    for (std::size_t index = 0; index < targets.size(); ++index) {
        candidateIdentities.push_back(tokenSource.registerOrUpdate(
            static_cast<int>(1'000 + index), &targets[index],
            kReadEvent | kWriteEvent).identity);
    }

    std::vector<NativeReadinessEvent> baselineNative;
    std::vector<NativeReadinessEvent> candidateNative;
    baselineNative.reserve(targets.size() * duplicateFactor);
    candidateNative.reserve(targets.size() * duplicateFactor);
    for (std::size_t repetition = 0; repetition < duplicateFactor; ++repetition) {
        for (std::size_t index = 0; index < targets.size(); ++index) {
            const auto events = repetition % 2 == 0 ? kReadEvent : kWriteEvent;
            baselineNative.push_back({
                .token = baselineToken(
                    static_cast<int>(1'000 + index),
                    static_cast<std::uint32_t>(index + 1)),
                .events = events,
            });
            candidateNative.push_back({
                .token = candidateIdentities[index].token(),
                .events = events,
            });
        }
    }
    for (std::size_t index = 0; index < candidateIdentities.size(); ++index) {
        (void)tokenSource.cancel(candidateIdentities[index], &targets[index]);
    }
    tokenSource.beginStop();
    if (!tokenSource.tryShutdown()) throw std::runtime_error("token fixture residue");

    // One unrecorded warmup per path. This remains development evidence.
    const auto baselineWarmup = runBaseline(targets, baselineNative, 1);
    const auto candidateWarmup = runCandidate(targets, candidateNative, 1);
    if (!baselineWarmup.settled || !candidateWarmup.settled ||
        baselineWarmup.checksum != candidateWarmup.checksum) {
        throw std::runtime_error("HP3 warmup invariant failed");
    }

    ScenarioResult result;
    result.baseline = runBaseline(targets, baselineNative, options.iterations);
    result.candidate = runCandidate(targets, candidateNative, options.iterations);
    result.valid = result.baseline.settled && result.candidate.settled &&
        result.baseline.checksum == result.candidate.checksum &&
        result.baseline.nativeEvents == result.candidate.nativeEvents &&
        result.baseline.deliveredNotices == result.candidate.deliveredNotices &&
        result.candidate.mapLookups == 0 && result.candidate.mergeProbes == 0 &&
        result.candidate.staleEvents == 0 && result.candidate.filteredEvents == 0;
    return result;
}

void writeRun(std::string_view name, const DecodeRun& run) {
    std::cout << "      \"" << name << "\": {\n"
              << "        \"elapsed_ns\": " << run.elapsedNs << ",\n"
              << "        \"native_events\": " << run.nativeEvents << ",\n"
              << "        \"delivered_notices\": " << run.deliveredNotices << ",\n"
              << "        \"wait_hash_lookups\": " << run.mapLookups << ",\n"
              << "        \"merge_probes\": " << run.mergeProbes << ",\n"
              << "        \"slot_probes\": " << run.slotProbes << ",\n"
              << "        \"stale_events\": " << run.staleEvents << ",\n"
              << "        \"filtered_events\": " << run.filteredEvents << ",\n"
              << "        \"checksum\": " << run.checksum << ",\n"
              << "        \"shutdown_residue\": " << (run.settled ? 0 : 1) << "\n"
              << "      }";
}

void writeScenario(
    std::string_view name,
    const ScenarioResult& result,
    bool trailingComma) {
    std::cout << "    \"" << name << "\": {\n";
    writeRun("current_fd_map_linear_merge", result.baseline);
    std::cout << ",\n";
    writeRun("slot_index_generation", result.candidate);
    std::cout << ",\n      \"valid\": " << (result.valid ? "true" : "false")
              << "\n    }" << (trailingComma ? "," : "") << "\n";
}

int run(int argc, char* argv[]) {
    const auto options = parseOptions(argc, argv);
    const auto unique = runScenario(options, 1);
    const auto duplicate = runScenario(options, options.duplicateFactor);
    const bool valid = unique.valid && duplicate.valid;

    std::cout << "{\n"
              << "  \"schema\": \"gamenet.hp3_epoll_slot_dispatch_prestudy.v1\",\n"
              << "  \"evidence_class\": \"development-only-portable-decoder\",\n"
              << "  \"native_epoll_evidence\": false,\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"active_sources\": " << options.activeSources << ",\n"
              << "  \"iterations\": " << options.iterations << ",\n"
              << "  \"duplicate_factor\": " << options.duplicateFactor << ",\n"
              << "  \"scenarios\": {\n";
    writeScenario("unique", unique, true);
    writeScenario("duplicate", duplicate, false);
    std::cout << "  },\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    return valid ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "gamenet_hp3_epoll_slot_dispatch_benchmark: "
                  << error.what() << '\n';
        return 2;
    }
}
