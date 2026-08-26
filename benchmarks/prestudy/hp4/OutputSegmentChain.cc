// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp4/OutputSegmentChain.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gamenet::experimental::hp4 {

OutputSegmentChain::OutputSegmentChain(OutputSegmentChainOptions options)
    : ownerThread_(std::this_thread::get_id()), options_(options) {
    if (options_.maxSegments == 0 || options_.maxPendingBytes == 0 ||
        options_.maxBatchSegments == 0 ||
        options_.maxBatchSegments > kMaximumBatchSegments ||
        options_.maxBatchBytes == 0 ||
        options_.maxBatchBytes > kMaximumBatchBytes) {
        throw std::invalid_argument("OutputSegmentChain requires coherent finite bounds");
    }
    segments_.resize(options_.maxSegments);
}

SegmentAdmissionStatus OutputSegmentChain::enqueueOwned(std::string&& bytes) {
    assertOwnerThread();
    if (!accepting_) return SegmentAdmissionStatus::Stopped;
    if (bytes.empty()) return SegmentAdmissionStatus::Accepted;
    if (count_ == segments_.size()) {
        ++rejectedSegments_;
        return SegmentAdmissionStatus::SegmentLimit;
    }
    if (bytes.size() > options_.maxPendingBytes - pendingBytes_) {
        ++rejectedBytes_;
        return SegmentAdmissionStatus::ByteLimit;
    }

    const auto length = bytes.size();
    const auto tail = (head_ + count_) % segments_.size();
    segments_[tail] = Segment{
        .owned = std::move(bytes),
        .length = length,
    };
    ++count_;
    pendingBytes_ += length;
    ++acceptedSegments_;
    acceptedBytes_ += length;
    highWaterSegments_ = (std::max)(highWaterSegments_, count_);
    highWaterBytes_ = (std::max)(highWaterBytes_, pendingBytes_);
    return SegmentAdmissionStatus::Accepted;
}

SegmentAdmissionStatus OutputSegmentChain::enqueueShared(
    std::shared_ptr<const std::string> bytes,
    std::size_t offset,
    std::size_t length) {
    assertOwnerThread();
    if (!accepting_) return SegmentAdmissionStatus::Stopped;
    if (!bytes || offset > bytes->size() || length > bytes->size() - offset) {
        return SegmentAdmissionStatus::Invalid;
    }
    if (length == 0) return SegmentAdmissionStatus::Accepted;
    if (count_ == segments_.size()) {
        ++rejectedSegments_;
        return SegmentAdmissionStatus::SegmentLimit;
    }
    if (length > options_.maxPendingBytes - pendingBytes_) {
        ++rejectedBytes_;
        return SegmentAdmissionStatus::ByteLimit;
    }

    const auto tail = (head_ + count_) % segments_.size();
    segments_[tail] = Segment{
        .shared = std::move(bytes),
        .sliceOffset = offset,
        .length = length,
    };
    ++count_;
    pendingBytes_ += length;
    ++acceptedSegments_;
    acceptedBytes_ += length;
    highWaterSegments_ = (std::max)(highWaterSegments_, count_);
    highWaterBytes_ = (std::max)(highWaterBytes_, pendingBytes_);
    return SegmentAdmissionStatus::Accepted;
}

PreparedSegmentBatch OutputSegmentChain::prepareBatch() {
    assertOwnerThread();
    if (inFlight_) return {.status = SegmentBatchStatus::InFlight};
    if (count_ == 0) return {.status = SegmentBatchStatus::Empty};

    PreparedSegmentBatch batch{.status = SegmentBatchStatus::Ready};
    std::size_t remainingBatchBytes = options_.maxBatchBytes;
    const auto segmentLimit = (std::min)(count_, options_.maxBatchSegments);
    for (std::size_t index = 0; index < segmentLimit && remainingBatchBytes != 0;
         ++index) {
        const Segment& segment = segments_[(head_ + index) % segments_.size()];
        const auto viewBytes = (std::min)(segment.remaining(), remainingBatchBytes);
        batch.views[batch.viewCount++] = {
            .data = segment.data(),
            .size = viewBytes,
        };
        batch.totalBytes += viewBytes;
        remainingBatchBytes -= viewBytes;
    }
    if (batch.totalBytes == 0) {
        throw std::logic_error("OutputSegmentChain prepared an empty non-empty batch");
    }
    submittedBytes_ = batch.totalBytes;
    inFlight_ = true;
    ++preparedBatches_;
    return batch;
}

SegmentCompletionResult OutputSegmentChain::completeBatch(std::size_t bytes) {
    assertOwnerThread();
    if (!inFlight_) return {.status = SegmentCompletionStatus::NoBatch};
    if (bytes == 0 || bytes > submittedBytes_) {
        return {
            .status = SegmentCompletionStatus::InvalidBytes,
            .remainingBytes = pendingBytes_,
        };
    }

    std::size_t remaining = bytes;
    while (remaining != 0) {
        Segment& segment = segments_[head_];
        const auto consumed = (std::min)(remaining, segment.remaining());
        segment.offset += consumed;
        remaining -= consumed;
        pendingBytes_ -= consumed;
        completedBytes_ += consumed;
        if (segment.remaining() == 0) popFrontCompleted();
    }
    submittedBytes_ = 0;
    inFlight_ = false;
    return {
        .status = SegmentCompletionStatus::Accepted,
        .completedBytes = bytes,
        .remainingBytes = pendingBytes_,
    };
}

bool OutputSegmentChain::failBatch() {
    assertOwnerThread();
    if (!inFlight_) return false;
    submittedBytes_ = 0;
    inFlight_ = false;
    ++failedBatches_;
    return true;
}

void OutputSegmentChain::beginStop() {
    assertOwnerThread();
    accepting_ = false;
}

SegmentCancelResult OutputSegmentChain::cancelRemaining() {
    assertOwnerThread();
    if (inFlight_) return {.status = SegmentCancelStatus::InFlight};
    SegmentCancelResult result{
        .status = SegmentCancelStatus::Cancelled,
        .discardedSegments = count_,
        .discardedBytes = pendingBytes_,
    };
    discardedSegments_ += count_;
    discardedBytes_ += pendingBytes_;
    while (count_ != 0) {
        segments_[head_] = Segment{};
        head_ = (head_ + 1) % segments_.size();
        --count_;
    }
    pendingBytes_ = 0;
    return result;
}

bool OutputSegmentChain::tryShutdown() {
    assertOwnerThread();
    if (accepting_ || inFlight_ || count_ != 0 || pendingBytes_ != 0) return false;
    shutdown_ = true;
    return true;
}

OutputSegmentSnapshot OutputSegmentChain::snapshot() const {
    assertOwnerThread();
    return {
        .capacitySegments = segments_.size(),
        .pendingSegments = count_,
        .pendingBytes = pendingBytes_,
        .highWaterSegments = highWaterSegments_,
        .highWaterBytes = highWaterBytes_,
        .acceptedSegments = acceptedSegments_,
        .acceptedBytes = acceptedBytes_,
        .completedSegments = completedSegments_,
        .completedBytes = completedBytes_,
        .discardedSegments = discardedSegments_,
        .discardedBytes = discardedBytes_,
        .preparedBatches = preparedBatches_,
        .failedBatches = failedBatches_,
        .rejectedSegments = rejectedSegments_,
        .rejectedBytes = rejectedBytes_,
        .accepting = accepting_,
        .inFlight = inFlight_,
        .shutdown = shutdown_,
    };
}

const char* OutputSegmentChain::Segment::data() const noexcept {
    const char* base = shared ? shared->data() + sliceOffset : owned.data();
    return base + offset;
}

std::size_t OutputSegmentChain::Segment::remaining() const noexcept {
    return length - offset;
}

void OutputSegmentChain::assertOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("OutputSegmentChain requires its constructing owner");
    }
}

void OutputSegmentChain::popFrontCompleted() {
    segments_[head_] = Segment{};
    head_ = (head_ + 1) % segments_.size();
    --count_;
    ++completedSegments_;
}

BroadcastSegmentPublishResult publishSharedOwnerBatch(
    std::shared_ptr<const std::string> bytes,
    std::size_t offset,
    std::size_t length,
    std::span<const BroadcastSegmentTarget> targets) {
    BroadcastSegmentPublishResult result;
    if (!targets.empty()) result.batchPublications = 1;
    for (const auto& target : targets) {
        if (target.chain == nullptr || target.expectedGeneration == 0 ||
            target.expectedGeneration != target.currentGeneration) {
            ++result.staleTargets;
            continue;
        }
        const auto status = target.chain->enqueueShared(bytes, offset, length);
        if (status == SegmentAdmissionStatus::Accepted) {
            ++result.acceptedTargets;
        } else {
            ++result.rejectedTargets;
        }
    }
    return result;
}

}  // namespace gamenet::experimental::hp4
