// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp5/CreditLease.h"

#include "support/TestAssert.h"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace gamenet::experimental::hp5;

LoopCreditLeaseOptions options(
    std::size_t connections = 2,
    std::size_t loopLimit = 128,
    std::size_t quantum = 64,
    std::size_t retained = 64) {
    return LoopCreditLeaseOptions{
        .maxConnections = connections,
        .loopHardLimitBytes = loopLimit,
        .leaseQuantumBytes = quantum,
        .retainedIdleCreditBytes = retained,
    };
}

void settle(LoopCreditLease& lease) {
    GAMENET_TEST_ASSERT(lease.beginStop() == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(lease.cancelAll() == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(lease.tryShutdown());
}

}  // namespace

int main() {
    using namespace gamenet::experimental::hp5;

    bool rejectedOptions = false;
    try {
        LoopCreditLeaseOptions invalid{
            .maxConnections = 0,
            .loopHardLimitBytes = 1,
            .leaseQuantumBytes = 1,
            .retainedIdleCreditBytes = 0,
        };
        invalid.validate();
    } catch (const std::invalid_argument&) {
        rejectedOptions = true;
    }
    GAMENET_TEST_ASSERT(rejectedOptions);

    bool rejectedBudget = false;
    try {
        SharedCreditBudget invalid(0);
    } catch (const std::invalid_argument&) {
        rejectedBudget = true;
    }
    GAMENET_TEST_ASSERT(rejectedBudget);

    auto server = std::make_shared<SharedCreditBudget>(512);
    auto global = std::make_shared<SharedCreditBudget>(512);
    LoopCreditLease lease(options(), server, global);
    const auto first = lease.registerConnection(96);
    const auto second = lease.registerConnection(96);
    GAMENET_TEST_ASSERT(first.status == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(second.status == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(
        lease.registerConnection(64).status == CreditStatus::NoConnectionSlot);
    GAMENET_TEST_ASSERT(lease.tryReserve(first.handle, 0) == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(lease.tryReserve(first.handle, 97) == CreditStatus::ConnectionLimit);
    GAMENET_TEST_ASSERT(lease.tryReserve(first.handle, 80) == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(lease.tryReserve(second.handle, 49) == CreditStatus::LoopLimit);
    GAMENET_TEST_ASSERT(lease.release(first.handle, 81) == CreditStatus::Invalid);
    GAMENET_TEST_ASSERT(lease.release(first.handle, 80) == CreditStatus::Accepted);

    const auto operationsAfterFirst = server->snapshot().modeledAtomicMutations;
    GAMENET_TEST_ASSERT(lease.tryReserve(second.handle, 32) == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(lease.release(second.handle, 32) == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(
        server->snapshot().modeledAtomicMutations == operationsAfterFirst);
    GAMENET_TEST_ASSERT(lease.trimIdleCredit() == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(server->snapshot().reservedBytes == 64);

    GAMENET_TEST_ASSERT(lease.retireConnection(first.handle) == CreditStatus::Accepted);
    const auto replacement = lease.registerConnection(64);
    GAMENET_TEST_ASSERT(replacement.status == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(replacement.handle.index == first.handle.index);
    GAMENET_TEST_ASSERT(replacement.handle.generation != first.handle.generation);
    GAMENET_TEST_ASSERT(
        lease.tryReserve(first.handle, 1) == CreditStatus::StaleHandle);
    GAMENET_TEST_ASSERT(
        lease.tryReserve(replacement.handle, 48) == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(
        lease.retireConnection(replacement.handle) == CreditStatus::PendingBytes);

    std::atomic<CreditStatus> foreignStatus{CreditStatus::Invalid};
    std::jthread foreign([&] {
        foreignStatus.store(
            lease.release(replacement.handle, 1), std::memory_order_release);
    });
    foreign.join();
    GAMENET_TEST_ASSERT(
        foreignStatus.load(std::memory_order_acquire) == CreditStatus::WrongOwner);
    GAMENET_TEST_ASSERT(lease.cancel(replacement.handle) == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(lease.retireConnection(replacement.handle) == CreditStatus::Accepted);
    GAMENET_TEST_ASSERT(lease.retireConnection(second.handle) == CreditStatus::Accepted);
    settle(lease);
    const auto settled = lease.snapshot();
    GAMENET_TEST_ASSERT(settled.acceptedBytes == 160);
    GAMENET_TEST_ASSERT(settled.releasedBytes == 112);
    GAMENET_TEST_ASSERT(settled.discardedBytes == 48);
    GAMENET_TEST_ASSERT(server->snapshot().reservedBytes == 0);
    GAMENET_TEST_ASSERT(global->snapshot().reservedBytes == 0);

    // A later global rejection rolls back the already acquired server scope.
    auto rollbackServer = std::make_shared<SharedCreditBudget>(256);
    auto smallGlobal = std::make_shared<SharedCreditBudget>(32);
    LoopCreditLease rollbackLease(
        options(1, 128, 64, 0), rollbackServer, smallGlobal);
    const auto rollbackConnection = rollbackLease.registerConnection(128);
    GAMENET_TEST_ASSERT(
        rollbackLease.tryReserve(rollbackConnection.handle, 8) ==
        CreditStatus::GlobalLimit);
    GAMENET_TEST_ASSERT(rollbackServer->snapshot().reservedBytes == 0);
    GAMENET_TEST_ASSERT(smallGlobal->snapshot().reservedBytes == 0);
    settle(rollbackLease);

    auto smallServer = std::make_shared<SharedCreditBudget>(32);
    auto ampleGlobal = std::make_shared<SharedCreditBudget>(256);
    LoopCreditLease serverLimited(
        options(1, 128, 64, 0), smallServer, ampleGlobal);
    const auto serverConnection = serverLimited.registerConnection(128);
    GAMENET_TEST_ASSERT(
        serverLimited.tryReserve(serverConnection.handle, 8) ==
        CreditStatus::ServerLimit);
    GAMENET_TEST_ASSERT(ampleGlobal->snapshot().reservedBytes == 0);
    settle(serverLimited);

    // Independent owner leases contend on shared parents without overshoot.
    constexpr std::size_t workerCount = 8;
    auto concurrentServer = std::make_shared<SharedCreditBudget>(256);
    auto concurrentGlobal = std::make_shared<SharedCreditBudget>(256);
    std::barrier start(static_cast<std::ptrdiff_t>(workerCount + 1));
    std::barrier observed(static_cast<std::ptrdiff_t>(workerCount + 1));
    std::barrier finish(static_cast<std::ptrdiff_t>(workerCount + 1));
    std::atomic<std::size_t> acceptedWorkers{0};
    std::vector<std::jthread> workers;
    workers.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
        workers.emplace_back([&] {
            LoopCreditLease workerLease(
                options(1, 64, 64, 64), concurrentServer, concurrentGlobal);
            const auto connection = workerLease.registerConnection(64);
            start.arrive_and_wait();
            const auto status = workerLease.tryReserve(connection.handle, 1);
            if (status == CreditStatus::Accepted) {
                acceptedWorkers.fetch_add(1, std::memory_order_relaxed);
            }
            observed.arrive_and_wait();
            finish.arrive_and_wait();
            if (status == CreditStatus::Accepted) {
                GAMENET_TEST_ASSERT(
                    workerLease.release(connection.handle, 1) == CreditStatus::Accepted);
            }
            settle(workerLease);
        });
    }
    start.arrive_and_wait();
    observed.arrive_and_wait();
    GAMENET_TEST_ASSERT(acceptedWorkers.load(std::memory_order_relaxed) == 4);
    GAMENET_TEST_ASSERT(concurrentServer->snapshot().reservedBytes == 256);
    GAMENET_TEST_ASSERT(concurrentGlobal->snapshot().reservedBytes == 256);
    GAMENET_TEST_ASSERT(concurrentServer->snapshot().peakReservedBytes <= 256);
    GAMENET_TEST_ASSERT(concurrentGlobal->snapshot().peakReservedBytes <= 256);
    finish.arrive_and_wait();
    workers.clear();
    GAMENET_TEST_ASSERT(concurrentServer->snapshot().reservedBytes == 0);
    GAMENET_TEST_ASSERT(concurrentGlobal->snapshot().reservedBytes == 0);

    return 0;
}
