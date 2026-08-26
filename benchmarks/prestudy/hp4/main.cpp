// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp4/OutputSegmentChain.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace gamenet::experimental::hp4;

struct Options {
    std::size_t endpoints{256};
    std::size_t iterations{500};
    std::size_t payloadBytes{1'024};
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
        if (argument == "--endpoints") {
            options.endpoints = parsePositive(value(), argument);
        } else if (argument == "--iterations") {
            options.iterations = parsePositive(value(), argument);
        } else if (argument == "--payload-bytes") {
            options.payloadBytes = parsePositive(value(), argument);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

struct RunMetrics {
    std::uint64_t elapsedNs{};
    std::uint64_t checksum{};
    std::uint64_t copiedBytes{};
    std::uint64_t allocationEvents{};
    std::uint64_t batches{};
    std::uint64_t views{};
    bool settled{};
};

std::uint64_t observation(
    std::size_t bytes,
    char first,
    char last) noexcept {
    return static_cast<std::uint64_t>(bytes) * 131U +
        static_cast<unsigned char>(first) * 17U +
        static_cast<unsigned char>(last);
}

RunMetrics runConcatenationBaseline(
    const Options& options,
    std::string_view header,
    const std::string& payload) {
    RunMetrics metrics;
    const auto started = Clock::now();
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        for (std::size_t endpoint = 0; endpoint < options.endpoints; ++endpoint) {
            std::string contiguous;
            contiguous.reserve(header.size() + payload.size());
            ++metrics.allocationEvents;
            contiguous.append(header);
            contiguous.append(payload);
            metrics.copiedBytes += contiguous.size();
            metrics.checksum += observation(
                contiguous.size(), contiguous.front(), contiguous.back());
            ++metrics.batches;
            ++metrics.views;
        }
    }
    metrics.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    metrics.settled = true;
    return metrics;
}

RunMetrics runSharedSegmentCandidate(
    const Options& options,
    std::shared_ptr<const std::string> header,
    std::shared_ptr<const std::string> payload) {
    std::vector<std::unique_ptr<OutputSegmentChain>> chains;
    chains.reserve(options.endpoints);
    for (std::size_t endpoint = 0; endpoint < options.endpoints; ++endpoint) {
        chains.push_back(std::make_unique<OutputSegmentChain>(OutputSegmentChainOptions{
            .maxSegments = 2,
            .maxPendingBytes = header->size() + payload->size(),
            .maxBatchSegments = 2,
            .maxBatchBytes = (std::min)(
                kMaximumBatchBytes, header->size() + payload->size()),
        }));
    }

    RunMetrics metrics;
    const auto started = Clock::now();
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        for (const auto& chain : chains) {
            if (chain->enqueueShared(header, 0, header->size()) !=
                    SegmentAdmissionStatus::Accepted ||
                chain->enqueueShared(payload, 0, payload->size()) !=
                    SegmentAdmissionStatus::Accepted) {
                throw std::runtime_error("candidate segment admission failed");
            }
            const auto batch = chain->prepareBatch();
            if (batch.status != SegmentBatchStatus::Ready || batch.viewCount != 2 ||
                batch.totalBytes != header->size() + payload->size()) {
                throw std::runtime_error("candidate batch shape failed");
            }
            metrics.checksum += observation(
                batch.totalBytes,
                batch.views[0].data[0],
                batch.views[1].data[batch.views[1].size - 1]);
            ++metrics.batches;
            metrics.views += batch.viewCount;
            if (chain->completeBatch(batch.totalBytes).status !=
                SegmentCompletionStatus::Accepted) {
                throw std::runtime_error("candidate completion failed");
            }
        }
    }
    metrics.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    metrics.copiedBytes = 0;
    metrics.allocationEvents = 0;
    metrics.settled = true;
    for (const auto& chain : chains) {
        chain->beginStop();
        (void)chain->cancelRemaining();
        metrics.settled = metrics.settled && chain->tryShutdown() &&
            chain->snapshot().settled();
    }
    return metrics;
}

void writeRun(std::string_view name, const RunMetrics& metrics) {
    std::cout << "    \"" << name << "\": {\n"
              << "      \"elapsed_ns\": " << metrics.elapsedNs << ",\n"
              << "      \"copied_bytes\": " << metrics.copiedBytes << ",\n"
              << "      \"allocation_events\": " << metrics.allocationEvents << ",\n"
              << "      \"batches\": " << metrics.batches << ",\n"
              << "      \"views\": " << metrics.views << ",\n"
              << "      \"checksum\": " << metrics.checksum << ",\n"
              << "      \"shutdown_residue\": " << (metrics.settled ? 0 : 1) << "\n"
              << "    }";
}

int run(int argc, char* argv[]) {
    const auto options = parseOptions(argc, argv);
    if (options.payloadBytes + 4 > kMaximumBatchBytes) {
        throw std::invalid_argument("payload plus header must fit the 64 KiB batch");
    }
    auto header = std::make_shared<const std::string>("HEAD");
    auto payload = std::make_shared<const std::string>(
        std::string(options.payloadBytes, 'p'));

    Options warmup = options;
    warmup.iterations = 1;
    const auto warmupBaseline = runConcatenationBaseline(warmup, *header, *payload);
    const auto warmupCandidate = runSharedSegmentCandidate(warmup, header, payload);
    if (warmupBaseline.checksum != warmupCandidate.checksum ||
        !warmupCandidate.settled) {
        throw std::runtime_error("HP4 warmup invariant failed");
    }

    const auto baseline = runConcatenationBaseline(options, *header, *payload);
    const auto candidate = runSharedSegmentCandidate(options, header, payload);
    const bool valid = baseline.settled && candidate.settled &&
        baseline.checksum == candidate.checksum &&
        baseline.batches == candidate.batches && candidate.copiedBytes == 0 &&
        candidate.allocationEvents == 0 && candidate.views == candidate.batches * 2;

    std::cout << "{\n"
              << "  \"schema\": \"gamenet.hp4_output_segment_chain_prestudy.v1\",\n"
              << "  \"evidence_class\": \"development-only-no-syscall\",\n"
              << "  \"native_vectored_send_evidence\": false,\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"endpoints\": " << options.endpoints << ",\n"
              << "  \"iterations\": " << options.iterations << ",\n"
              << "  \"payload_bytes\": " << options.payloadBytes << ",\n"
              << "  \"runs\": {\n";
    writeRun("concatenate_per_endpoint", baseline);
    std::cout << ",\n";
    writeRun("shared_two_segment", candidate);
    std::cout << "\n  },\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    return valid ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "gamenet_hp4_output_segment_chain_benchmark: "
                  << error.what() << '\n';
        return 2;
    }
}
