// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerView.h"

#include "gamenet/protocol/PacketFramer.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using gamenet::experimental::hp1::FrameVisitStatus;
using gamenet::experimental::hp1::PacketFramerViewPrototype;
using gamenet::experimental::hp1::PacketView;

std::atomic<std::uint64_t> allocationCount{};
thread_local bool countAllocations = false;

struct Config {
    std::size_t iterations{20000};
    std::size_t frames{32};
    std::size_t payloadBytes{256};
};

struct Measurement {
    std::uint64_t elapsedNs{};
    std::uint64_t checksum{};
    std::uint64_t allocations{};
    std::uint64_t payloadBytes{};
    std::uint64_t copiedBytes{};
    std::uint64_t retainedBytes{};
    std::uint64_t frames{};
};

std::size_t parseSize(std::string_view text, std::string_view option) {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed == 0 ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string(option) + " requires a positive integer");
    }
    return static_cast<std::size_t>(parsed);
}

Config parseArgs(int argc, char** argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help") {
            std::cout
                << "Usage: gamenet_hp1_packet_framer_view_benchmark "
                   "[--iterations N] [--frames N] [--payload-bytes N]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(option) + " requires a value");
        }
        const std::string_view value(argv[++index]);
        if (option == "--iterations") {
            config.iterations = parseSize(value, option);
        } else if (option == "--frames") {
            config.frames = parseSize(value, option);
        } else if (option == "--payload-bytes") {
            config.payloadBytes = parseSize(value, option);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(option));
        }
    }
    if (config.payloadBytes > 1024U * 1024U || config.frames > 4096U ||
        config.iterations > 100000000U) {
        throw std::invalid_argument("benchmark parameters exceed prestudy safety bounds");
    }
    return config;
}

void appendFrame(std::string& output, std::string_view payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    output.push_back(static_cast<char>((size >> 24U) & 0xffU));
    output.push_back(static_cast<char>((size >> 16U) & 0xffU));
    output.push_back(static_cast<char>((size >> 8U) & 0xffU));
    output.push_back(static_cast<char>(size & 0xffU));
    output.append(payload);
}

std::uint64_t mix(std::uint64_t checksum, std::string_view payload) noexcept {
    for (const unsigned char value : payload) {
        checksum ^= value;
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

Measurement runLegacy(
    const Config& config,
    const gamenet::protocol::PacketFramerOptions& options,
    std::string_view wire) {
    gamenet::protocol::PacketFramer framer(options);
    Measurement measurement{
        .checksum = 1469598103934665603ULL,
        .payloadBytes = static_cast<std::uint64_t>(config.iterations) *
            config.frames * config.payloadBytes,
        .copiedBytes = static_cast<std::uint64_t>(config.iterations) *
            config.frames * config.payloadBytes,
        .frames = static_cast<std::uint64_t>(config.iterations) * config.frames,
    };

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations = true;
    const auto start = Clock::now();
    for (std::size_t iteration = 0; iteration < config.iterations; ++iteration) {
        auto result = framer.push(wire);
        for (;;) {
            for (const auto& payload : result.frames) {
                measurement.checksum = mix(measurement.checksum, payload);
            }
            if (!result.needsContinuation) break;
            result = framer.push({});
        }
    }
    const auto finish = Clock::now();
    countAllocations = false;
    measurement.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    measurement.allocations = allocationCount.load(std::memory_order_relaxed);
    return measurement;
}

Measurement runCandidate(
    const Config& config,
    const gamenet::protocol::PacketFramerOptions& options,
    std::string_view wire) {
    PacketFramerViewPrototype framer(options);
    const std::span bytes(
        reinterpret_cast<const std::byte*>(wire.data()),
        wire.size());
    Measurement measurement{
        .checksum = 1469598103934665603ULL,
        .payloadBytes = static_cast<std::uint64_t>(config.iterations) *
            config.frames * config.payloadBytes,
        .copiedBytes = 0,
        .retainedBytes = 0,
        .frames = static_cast<std::uint64_t>(config.iterations) * config.frames,
    };

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations = true;
    const auto start = Clock::now();
    for (std::size_t iteration = 0; iteration < config.iterations; ++iteration) {
        std::size_t consumed = 0;
        for (;;) {
            const auto result = framer.visitFrames(
                bytes.subspan(consumed),
                [&](PacketView view) noexcept {
                    measurement.checksum = mix(measurement.checksum, view.asStringView());
                });
            if (result.status != FrameVisitStatus::FramesVisited &&
                result.status != FrameVisitStatus::BudgetExhausted) {
                throw std::runtime_error("candidate visit failed semantic validation");
            }
            consumed += result.consumedBytes;
            if (!result.needsContinuation) break;
        }
        if (consumed != bytes.size()) {
            throw std::runtime_error("candidate did not consume the complete generated stream");
        }
    }
    const auto finish = Clock::now();
    countAllocations = false;
    measurement.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    measurement.allocations = allocationCount.load(std::memory_order_relaxed);
    return measurement;
}

void printMeasurement(std::string_view name, const Measurement& value, bool trailingComma) {
    std::cout
        << "    \"" << name << "\": {\n"
        << "      \"elapsed_ns\": " << value.elapsedNs << ",\n"
        << "      \"checksum\": " << value.checksum << ",\n"
        << "      \"allocations\": " << value.allocations << ",\n"
        << "      \"payload_bytes\": " << value.payloadBytes << ",\n"
        << "      \"copied_bytes\": " << value.copiedBytes << ",\n"
        << "      \"retained_bytes\": " << value.retainedBytes << ",\n"
        << "      \"frames\": " << value.frames << "\n"
        << "    }" << (trailingComma ? "," : "") << "\n";
}

}  // namespace

void* operator new(std::size_t size) {
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        if (countAllocations) allocationCount.fetch_add(1, std::memory_order_relaxed);
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main(int argc, char** argv) {
    try {
        const auto config = parseArgs(argc, argv);
        const auto maximumFrameBytes = sizeof(std::uint32_t) + config.payloadBytes;
        const auto wireBytes = maximumFrameBytes * config.frames;
        if (wireBytes < maximumFrameBytes) {
            throw std::invalid_argument("wire size overflow");
        }

        std::string payload(config.payloadBytes, '\0');
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<char>((index * 131U + 17U) & 0xffU);
        }
        std::string wire;
        wire.reserve(wireBytes);
        for (std::size_t index = 0; index < config.frames; ++index) appendFrame(wire, payload);

        const gamenet::protocol::PacketFramerOptions options{
            .maxPayloadBytes = config.payloadBytes,
            .maxBufferedBytes = wireBytes,
            .maxFramesPerPush = config.frames,
            .maxFrameBytesPerPush = wireBytes,
        };
        const auto legacy = runLegacy(config, options, wire);
        const auto candidate = runCandidate(config, options, wire);
        if (legacy.checksum != candidate.checksum || legacy.frames != candidate.frames ||
            candidate.allocations != 0 || candidate.copiedBytes != 0) {
            throw std::runtime_error("legacy/candidate structural result mismatch");
        }

        std::cout
            << "{\n"
            << "  \"schema\": \"gamenet.hp1_packet_framer_view_prestudy.v1\",\n"
            << "  \"status\": \"ok\",\n"
            << "  \"evidence_class\": \"development\",\n"
            << "  \"promotion_eligible\": false,\n"
            << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
            << "  \"parameters\": {\n"
            << "    \"iterations\": " << config.iterations << ",\n"
            << "    \"frames\": " << config.frames << ",\n"
            << "    \"payload_bytes\": " << config.payloadBytes << "\n"
            << "  },\n"
            << "  \"paths\": {\n";
        printMeasurement("legacy_callback_copy", legacy, true);
        printMeasurement("candidate_borrowed_view", candidate, false);
        std::cout << "  }\n}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HP1 PacketFramer view benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
