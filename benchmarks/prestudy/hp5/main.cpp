// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp5/CreditLease.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace gamenet::experimental::hp5;

struct Options {
    std::size_t messages{200'000};
    std::size_t payloadBytes{512};
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
        if (argument == "--messages") {
            options.messages = parsePositive(value(), argument);
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
    std::uint64_t modeledAtomicMutations{};
    std::uint64_t acceptedBytes{};
    std::uint64_t releasedBytes{};
    std::uint64_t discardedBytes{};
    std::uint64_t checksum{};
    std::uint64_t refills{};
    std::uint64_t returns{};
    bool settled{};
};

RunMetrics runExactHierarchy(const Options& options) {
    constexpr std::size_t limit = 4U * 1024U * 1024U;
    std::array<SharedCreditBudget, 4> scopes{
        SharedCreditBudget(limit), SharedCreditBudget(limit),
        SharedCreditBudget(limit), SharedCreditBudget(limit)};
    RunMetrics metrics;
    const auto started = Clock::now();
    for (std::size_t message = 0; message < options.messages; ++message) {
        std::size_t acquired = 0;
        for (; acquired < scopes.size(); ++acquired) {
            if (!scopes[acquired].tryReserve(options.payloadBytes)) break;
        }
        if (acquired != scopes.size()) {
            while (acquired != 0) scopes[--acquired].release(options.payloadBytes);
            throw std::runtime_error("exact hierarchy unexpectedly rejected");
        }
        metrics.acceptedBytes += options.payloadBytes;
        metrics.checksum += (message + 1U) * 131U + options.payloadBytes;
        for (std::size_t index = scopes.size(); index != 0; --index) {
            scopes[index - 1].release(options.payloadBytes);
        }
        metrics.releasedBytes += options.payloadBytes;
    }
    metrics.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    metrics.settled = true;
    for (const auto& scope : scopes) {
        const auto snapshot = scope.snapshot();
        metrics.modeledAtomicMutations += snapshot.modeledAtomicMutations;
        metrics.settled = metrics.settled && snapshot.reservedBytes == 0;
    }
    return metrics;
}

RunMetrics runCreditLease(const Options& options) {
    constexpr std::size_t limit = 4U * 1024U * 1024U;
    auto server = std::make_shared<SharedCreditBudget>(limit);
    auto global = std::make_shared<SharedCreditBudget>(limit);
    LoopCreditLease lease(
        LoopCreditLeaseOptions{
            .maxConnections = 1,
            .loopHardLimitBytes = limit,
            .leaseQuantumBytes = 64U * 1024U,
            .retainedIdleCreditBytes = 64U * 1024U,
        },
        server,
        global);
    const auto connection = lease.registerConnection(limit);
    if (connection.status != CreditStatus::Accepted) {
        throw std::runtime_error("credit connection registration failed");
    }
    RunMetrics metrics;
    const auto started = Clock::now();
    for (std::size_t message = 0; message < options.messages; ++message) {
        if (lease.tryReserve(connection.handle, options.payloadBytes) !=
            CreditStatus::Accepted) {
            throw std::runtime_error("credit lease unexpectedly rejected");
        }
        metrics.checksum += (message + 1U) * 131U + options.payloadBytes;
        if (lease.release(connection.handle, options.payloadBytes) !=
            CreditStatus::Accepted) {
            throw std::runtime_error("credit lease release failed");
        }
    }
    if (lease.beginStop() != CreditStatus::Accepted ||
        lease.cancelAll() != CreditStatus::Accepted) {
        throw std::runtime_error("credit lease stop failed");
    }
    metrics.elapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    const auto leaseSnapshot = lease.snapshot();
    const auto serverSnapshot = server->snapshot();
    const auto globalSnapshot = global->snapshot();
    metrics.modeledAtomicMutations =
        serverSnapshot.modeledAtomicMutations + globalSnapshot.modeledAtomicMutations;
    metrics.acceptedBytes = leaseSnapshot.acceptedBytes;
    metrics.releasedBytes = leaseSnapshot.releasedBytes;
    metrics.discardedBytes = leaseSnapshot.discardedBytes;
    metrics.refills = leaseSnapshot.refillCount;
    metrics.returns = leaseSnapshot.returnCount;
    metrics.settled = lease.tryShutdown() &&
        serverSnapshot.reservedBytes == 0 && globalSnapshot.reservedBytes == 0;
    return metrics;
}

void writeRun(std::string_view name, const RunMetrics& metrics) {
    std::cout << "    \"" << name << "\": {\n"
              << "      \"elapsed_ns\": " << metrics.elapsedNs << ",\n"
              << "      \"modeled_atomic_mutations\": " << metrics.modeledAtomicMutations << ",\n"
              << "      \"accepted_bytes\": " << metrics.acceptedBytes << ",\n"
              << "      \"released_bytes\": " << metrics.releasedBytes << ",\n"
              << "      \"discarded_bytes\": " << metrics.discardedBytes << ",\n"
              << "      \"refills\": " << metrics.refills << ",\n"
              << "      \"returns\": " << metrics.returns << ",\n"
              << "      \"checksum\": " << metrics.checksum << ",\n"
              << "      \"shutdown_residue\": " << (metrics.settled ? 0 : 1) << "\n"
              << "    }";
}

int run(int argc, char* argv[]) {
    const auto options = parseOptions(argc, argv);
    if (options.payloadBytes > 64U * 1024U) {
        throw std::invalid_argument("payload must not exceed the 64 KiB lease quantum");
    }
    Options warmup = options;
    warmup.messages = 1;
    const auto warmupExact = runExactHierarchy(warmup);
    const auto warmupLease = runCreditLease(warmup);
    if (warmupExact.checksum != warmupLease.checksum || !warmupLease.settled) {
        throw std::runtime_error("HP5 warmup invariant failed");
    }

    const auto exact = runExactHierarchy(options);
    const auto candidate = runCreditLease(options);
    const bool valid = exact.settled && candidate.settled &&
        exact.checksum == candidate.checksum &&
        exact.acceptedBytes == candidate.acceptedBytes &&
        exact.releasedBytes == candidate.releasedBytes &&
        candidate.discardedBytes == 0 &&
        candidate.modeledAtomicMutations < exact.modeledAtomicMutations;

    std::cout << "{\n"
              << "  \"schema\": \"gamenet.hp5_credit_lease_prestudy.v1\",\n"
              << "  \"evidence_class\": \"development-only-modeled-atomic\",\n"
              << "  \"production_tcp_path\": false,\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"messages\": " << options.messages << ",\n"
              << "  \"payload_bytes\": " << options.payloadBytes << ",\n"
              << "  \"runs\": {\n";
    writeRun("exact_four_scope", exact);
    std::cout << ",\n";
    writeRun("owner_local_lease", candidate);
    std::cout << "\n  },\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    return valid ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "gamenet_hp5_credit_lease_benchmark: " << error.what() << '\n';
        return 2;
    }
}
