// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HP4 实验原型：owner-local fixed segment ring and portable vectored views。
// 不调用 socket，不构成 TcpConnection/TransportEndpoint 公共 API。

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace gamenet::experimental::hp4 {

inline constexpr std::size_t kMaximumBatchSegments = 16;
inline constexpr std::size_t kMaximumBatchBytes = 64 * 1024;

enum class SegmentAdmissionStatus {
    Accepted,
    SegmentLimit,
    ByteLimit,
    Stopped,
    Invalid,
};

enum class SegmentBatchStatus {
    Ready,
    Empty,
    InFlight,
};

enum class SegmentCompletionStatus {
    Accepted,
    NoBatch,
    InvalidBytes,
};

enum class SegmentCancelStatus {
    Cancelled,
    InFlight,
};

struct OutputSegmentChainOptions {
    std::size_t maxSegments{1'024};
    std::size_t maxPendingBytes{8 * 1024 * 1024};
    std::size_t maxBatchSegments{kMaximumBatchSegments};
    std::size_t maxBatchBytes{kMaximumBatchBytes};
};

struct SegmentView {
    const char* data{};
    std::size_t size{};
};

struct PreparedSegmentBatch {
    SegmentBatchStatus status{SegmentBatchStatus::Empty};
    std::array<SegmentView, kMaximumBatchSegments> views{};
    std::size_t viewCount{};
    std::size_t totalBytes{};
};

struct SegmentCompletionResult {
    SegmentCompletionStatus status{SegmentCompletionStatus::NoBatch};
    std::size_t completedBytes{};
    std::size_t remainingBytes{};
};

struct SegmentCancelResult {
    SegmentCancelStatus status{SegmentCancelStatus::Cancelled};
    std::size_t discardedSegments{};
    std::size_t discardedBytes{};
};

struct OutputSegmentSnapshot {
    std::size_t capacitySegments{};
    std::size_t pendingSegments{};
    std::size_t pendingBytes{};
    std::size_t highWaterSegments{};
    std::size_t highWaterBytes{};
    std::uint64_t acceptedSegments{};
    std::uint64_t acceptedBytes{};
    std::uint64_t completedSegments{};
    std::uint64_t completedBytes{};
    std::uint64_t discardedSegments{};
    std::uint64_t discardedBytes{};
    std::uint64_t preparedBatches{};
    std::uint64_t failedBatches{};
    std::uint64_t rejectedSegments{};
    std::uint64_t rejectedBytes{};
    bool accepting{};
    bool inFlight{};
    bool shutdown{};

    bool settled() const noexcept {
        return shutdown && pendingSegments == 0 && pendingBytes == 0 &&
            !inFlight && acceptedSegments == completedSegments + discardedSegments &&
            acceptedBytes == completedBytes + discardedBytes;
    }
};

class OutputSegmentChain {
public:
    explicit OutputSegmentChain(OutputSegmentChainOptions options = {});

    OutputSegmentChain(const OutputSegmentChain&) = delete;
    OutputSegmentChain& operator=(const OutputSegmentChain&) = delete;

    SegmentAdmissionStatus enqueueOwned(std::string&& bytes);
    SegmentAdmissionStatus enqueueShared(
        std::shared_ptr<const std::string> bytes,
        std::size_t offset,
        std::size_t length);

    PreparedSegmentBatch prepareBatch();
    SegmentCompletionResult completeBatch(std::size_t bytes);
    bool failBatch();

    void beginStop();
    SegmentCancelResult cancelRemaining();
    bool tryShutdown();
    OutputSegmentSnapshot snapshot() const;

private:
    struct Segment {
        std::string owned;
        std::shared_ptr<const std::string> shared;
        std::size_t sliceOffset{};
        std::size_t length{};
        std::size_t offset{};

        const char* data() const noexcept;
        std::size_t remaining() const noexcept;
        bool isShared() const noexcept { return static_cast<bool>(shared); }
    };

    void assertOwnerThread() const;
    void popFrontCompleted();

    const std::thread::id ownerThread_;
    OutputSegmentChainOptions options_;
    std::vector<Segment> segments_;
    std::size_t head_{};
    std::size_t count_{};
    std::size_t pendingBytes_{};
    std::size_t highWaterSegments_{};
    std::size_t highWaterBytes_{};
    std::size_t submittedBytes_{};
    bool accepting_{true};
    bool inFlight_{false};
    bool shutdown_{false};
    std::uint64_t acceptedSegments_{};
    std::uint64_t acceptedBytes_{};
    std::uint64_t completedSegments_{};
    std::uint64_t completedBytes_{};
    std::uint64_t discardedSegments_{};
    std::uint64_t discardedBytes_{};
    std::uint64_t preparedBatches_{};
    std::uint64_t failedBatches_{};
    std::uint64_t rejectedSegments_{};
    std::uint64_t rejectedBytes_{};
};

struct BroadcastSegmentTarget {
    OutputSegmentChain* chain{};
    std::uint64_t expectedGeneration{};
    std::uint64_t currentGeneration{};
};

struct BroadcastSegmentPublishResult {
    std::size_t batchPublications{};
    std::size_t acceptedTargets{};
    std::size_t staleTargets{};
    std::size_t rejectedTargets{};
};

BroadcastSegmentPublishResult publishSharedOwnerBatch(
    std::shared_ptr<const std::string> bytes,
    std::size_t offset,
    std::size_t length,
    std::span<const BroadcastSegmentTarget> targets);

}  // namespace gamenet::experimental::hp4
