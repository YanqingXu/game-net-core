// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp4/OutputSegmentChain.h"

#include "support/TestAssert.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace gamenet::experimental::hp4;

void settle(OutputSegmentChain& chain) {
    chain.beginStop();
    const auto cancelled = chain.cancelRemaining();
    GAMENET_TEST_ASSERT(cancelled.status == SegmentCancelStatus::Cancelled);
    GAMENET_TEST_ASSERT(chain.tryShutdown());
    GAMENET_TEST_ASSERT(chain.snapshot().settled());
}

void testOwnedAndSharedStorageWithoutConcatenation() {
    OutputSegmentChain chain({
        .maxSegments = 4,
        .maxPendingBytes = 16 * 1024,
        .maxBatchSegments = 4,
        .maxBatchBytes = 16 * 1024,
    });

    std::string owned(4'096, 'o');
    const char* ownedStorage = owned.data();
    GAMENET_TEST_ASSERT(
        chain.enqueueOwned(std::move(owned)) == SegmentAdmissionStatus::Accepted);
    auto shared = std::make_shared<const std::string>(std::string(4'096, 's'));
    const char* sharedStorage = shared->data() + 17;
    GAMENET_TEST_ASSERT(
        chain.enqueueShared(shared, 17, 2'048) == SegmentAdmissionStatus::Accepted);

    const auto batch = chain.prepareBatch();
    GAMENET_TEST_ASSERT(batch.status == SegmentBatchStatus::Ready);
    GAMENET_TEST_ASSERT(batch.viewCount == 2);
    GAMENET_TEST_ASSERT(batch.views[0].data == ownedStorage);
    GAMENET_TEST_ASSERT(batch.views[0].size == 4'096);
    GAMENET_TEST_ASSERT(batch.views[1].data == sharedStorage);
    GAMENET_TEST_ASSERT(batch.views[1].size == 2'048);
    GAMENET_TEST_ASSERT(
        chain.prepareBatch().status == SegmentBatchStatus::InFlight);

    const auto completed = chain.completeBatch(4'100);
    GAMENET_TEST_ASSERT(completed.status == SegmentCompletionStatus::Accepted);
    GAMENET_TEST_ASSERT(completed.remainingBytes == 2'044);
    const auto suffix = chain.prepareBatch();
    GAMENET_TEST_ASSERT(suffix.viewCount == 1);
    GAMENET_TEST_ASSERT(suffix.views[0].data == sharedStorage + 4);
    GAMENET_TEST_ASSERT(suffix.views[0].size == 2'044);
    GAMENET_TEST_ASSERT(
        chain.completeBatch(2'044).status == SegmentCompletionStatus::Accepted);

    chain.beginStop();
    GAMENET_TEST_ASSERT(chain.cancelRemaining().discardedBytes == 0);
    GAMENET_TEST_ASSERT(chain.tryShutdown());
    const auto snapshot = chain.snapshot();
    GAMENET_TEST_ASSERT(snapshot.acceptedSegments == 2);
    GAMENET_TEST_ASSERT(snapshot.completedSegments == 2);
    GAMENET_TEST_ASSERT(snapshot.acceptedBytes == 6'144);
    GAMENET_TEST_ASSERT(snapshot.completedBytes == 6'144);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

void testBatchLimitsFifoAndCrossSegmentPartialCompletion() {
    OutputSegmentChain chain({
        .maxSegments = 20,
        .maxPendingBytes = 100 * 1024,
        .maxBatchSegments = 16,
        .maxBatchBytes = 64 * 1024,
    });
    for (int index = 0; index < 20; ++index) {
        GAMENET_TEST_ASSERT(
            chain.enqueueOwned(std::string(4'096, static_cast<char>('a' + index))) ==
            SegmentAdmissionStatus::Accepted);
    }

    auto batch = chain.prepareBatch();
    GAMENET_TEST_ASSERT(batch.viewCount == 16);
    GAMENET_TEST_ASSERT(batch.totalBytes == 64 * 1024);
    for (std::size_t index = 0; index < batch.viewCount; ++index) {
        GAMENET_TEST_ASSERT(batch.views[index].data[0] == static_cast<char>('a' + index));
    }
    GAMENET_TEST_ASSERT(
        chain.completeBatch(4'096 * 15 + 123).status ==
        SegmentCompletionStatus::Accepted);
    batch = chain.prepareBatch();
    GAMENET_TEST_ASSERT(batch.views[0].data[0] == 'p');
    GAMENET_TEST_ASSERT(batch.views[0].size == 4'096 - 123);
    GAMENET_TEST_ASSERT(batch.views[1].data[0] == 'q');
    GAMENET_TEST_ASSERT(batch.viewCount == 5);
    GAMENET_TEST_ASSERT(
        chain.completeBatch(batch.totalBytes).status == SegmentCompletionStatus::Accepted);
    GAMENET_TEST_ASSERT(chain.snapshot().pendingBytes == 0);
    settle(chain);
}

void testFailureCapacityStopAndExactDiscard() {
    bool optionsRejected = false;
    try {
        OutputSegmentChain invalid({.maxSegments = 1, .maxBatchSegments = 17});
    } catch (const std::invalid_argument&) {
        optionsRejected = true;
    }
    GAMENET_TEST_ASSERT(optionsRejected);

    OutputSegmentChain immediate({
        .maxSegments = 1,
        .maxPendingBytes = 16,
        .maxBatchSegments = 1,
        .maxBatchBytes = 16,
    });
    GAMENET_TEST_ASSERT(
        immediate.enqueueOwned(std::string("direct")) == SegmentAdmissionStatus::Accepted);
    const auto direct = immediate.prepareBatch();
    GAMENET_TEST_ASSERT(direct.status == SegmentBatchStatus::Ready);
    GAMENET_TEST_ASSERT(direct.viewCount == 1);
    GAMENET_TEST_ASSERT(direct.totalBytes == 6);
    GAMENET_TEST_ASSERT(
        immediate.completeBatch(6).status == SegmentCompletionStatus::Accepted);
    settle(immediate);

    OutputSegmentChain chain({
        .maxSegments = 2,
        .maxPendingBytes = 8,
        .maxBatchSegments = 2,
        .maxBatchBytes = 8,
    });
    std::string first("1234");
    std::string second("5678");
    std::string overflow("x");
    GAMENET_TEST_ASSERT(
        chain.enqueueOwned(std::move(first)) == SegmentAdmissionStatus::Accepted);
    GAMENET_TEST_ASSERT(
        chain.enqueueOwned(std::move(second)) == SegmentAdmissionStatus::Accepted);
    GAMENET_TEST_ASSERT(
        chain.enqueueOwned(std::move(overflow)) == SegmentAdmissionStatus::SegmentLimit);
    GAMENET_TEST_ASSERT(overflow == "x");

    OutputSegmentChain byteLimited({
        .maxSegments = 2,
        .maxPendingBytes = 4,
        .maxBatchSegments = 2,
        .maxBatchBytes = 4,
    });
    std::string tooLarge("12345");
    GAMENET_TEST_ASSERT(
        byteLimited.enqueueOwned(std::move(tooLarge)) == SegmentAdmissionStatus::ByteLimit);
    GAMENET_TEST_ASSERT(tooLarge == "12345");
    GAMENET_TEST_ASSERT(
        byteLimited.enqueueShared({}, 0, 0) == SegmentAdmissionStatus::Invalid);
    settle(byteLimited);

    auto batch = chain.prepareBatch();
    GAMENET_TEST_ASSERT(batch.totalBytes == 8);
    GAMENET_TEST_ASSERT(
        chain.completeBatch(0).status == SegmentCompletionStatus::InvalidBytes);
    GAMENET_TEST_ASSERT(
        chain.completeBatch(9).status == SegmentCompletionStatus::InvalidBytes);
    chain.beginStop();
    GAMENET_TEST_ASSERT(
        chain.cancelRemaining().status == SegmentCancelStatus::InFlight);
    GAMENET_TEST_ASSERT(chain.failBatch());
    const auto cancelled = chain.cancelRemaining();
    GAMENET_TEST_ASSERT(cancelled.discardedSegments == 2);
    GAMENET_TEST_ASSERT(cancelled.discardedBytes == 8);
    GAMENET_TEST_ASSERT(
        chain.enqueueOwned(std::string("late")) == SegmentAdmissionStatus::Stopped);
    GAMENET_TEST_ASSERT(chain.tryShutdown());
    const auto snapshot = chain.snapshot();
    GAMENET_TEST_ASSERT(snapshot.acceptedBytes == 8);
    GAMENET_TEST_ASSERT(snapshot.discardedBytes == 8);
    GAMENET_TEST_ASSERT(snapshot.failedBatches == 1);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

void testSharedBroadcastGenerationAndOwnerAffinity() {
    OutputSegmentChain first({.maxSegments = 2, .maxPendingBytes = 1024});
    OutputSegmentChain second({.maxSegments = 2, .maxPendingBytes = 1024});
    OutputSegmentChain stale({.maxSegments = 2, .maxPendingBytes = 1024});
    auto payload = std::make_shared<const std::string>(std::string(512, 'b'));
    const auto before = payload.use_count();
    const BroadcastSegmentTarget targets[] = {
        {.chain = &first, .expectedGeneration = 7, .currentGeneration = 7},
        {.chain = &second, .expectedGeneration = 8, .currentGeneration = 8},
        {.chain = &stale, .expectedGeneration = 9, .currentGeneration = 10},
    };
    const auto published = publishSharedOwnerBatch(payload, 0, payload->size(), targets);
    GAMENET_TEST_ASSERT(published.batchPublications == 1);
    GAMENET_TEST_ASSERT(published.acceptedTargets == 2);
    GAMENET_TEST_ASSERT(published.staleTargets == 1);
    GAMENET_TEST_ASSERT(published.rejectedTargets == 0);
    GAMENET_TEST_ASSERT(payload.use_count() == before + 2);
    const auto firstBatch = first.prepareBatch();
    const auto secondBatch = second.prepareBatch();
    GAMENET_TEST_ASSERT(firstBatch.views[0].data == payload->data());
    GAMENET_TEST_ASSERT(secondBatch.views[0].data == payload->data());
    GAMENET_TEST_ASSERT(stale.snapshot().pendingSegments == 0);
    GAMENET_TEST_ASSERT(first.completeBatch(512).status == SegmentCompletionStatus::Accepted);
    GAMENET_TEST_ASSERT(second.completeBatch(512).status == SegmentCompletionStatus::Accepted);

    std::atomic<bool> foreignRejected{false};
    std::thread foreign([&] {
        try {
            (void)first.snapshot();
        } catch (const std::logic_error&) {
            foreignRejected.store(true, std::memory_order_release);
        }
    });
    foreign.join();
    GAMENET_TEST_ASSERT(foreignRejected.load(std::memory_order_acquire));
    settle(first);
    settle(second);
    settle(stale);
}

}  // namespace

int main() {
    testOwnedAndSharedStorageWithoutConcatenation();
    testBatchLimitsFifoAndCrossSegmentPartialCompletion();
    testFailureCapacityStopAndExactDiscard();
    testSharedBroadcastGenerationAndOwnerAffinity();
    return 0;
}
