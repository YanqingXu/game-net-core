// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HP1 实验原型：同步借用调用方 Buffer 的连续可读区，按现有 framing 预算访问帧。
// 本头文件只属于默认关闭且非安装的 benchmark/prestudy target，不构成公共 API。

#include "gamenet/protocol/PacketFramer.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace gamenet::experimental::hp1 {

class OwnedPacket {
public:
    explicit OwnedPacket(std::string bytes) : bytes_(std::move(bytes)) {}

    OwnedPacket(const OwnedPacket&) = delete;
    OwnedPacket& operator=(const OwnedPacket&) = delete;
    OwnedPacket(OwnedPacket&&) noexcept = default;
    OwnedPacket& operator=(OwnedPacket&&) noexcept = default;

    std::span<const std::byte> bytes() const noexcept {
        return {
            reinterpret_cast<const std::byte*>(bytes_.data()),
            bytes_.size(),
        };
    }

    std::string_view asStringView() const noexcept { return bytes_; }
    std::string releaseString() && noexcept { return std::move(bytes_); }

private:
    std::string bytes_;
};

class PacketView {
public:
    explicit PacketView(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    std::span<const std::byte> bytes() const noexcept { return bytes_; }
    std::string_view asStringView() const noexcept {
        return {
            reinterpret_cast<const char*>(bytes_.data()),
            bytes_.size(),
        };
    }
    OwnedPacket retain() const { return OwnedPacket(std::string(asStringView())); }

private:
    std::span<const std::byte> bytes_;
};

enum class FrameVisitStatus {
    NeedMoreData,
    FramesVisited,
    BudgetExhausted,
    FrameTooLarge,
    BufferLimitExceeded,
    Faulted,
    VisitorException,
    ReentrantVisitRejected,
};

struct FrameVisitResult {
    FrameVisitStatus status{FrameVisitStatus::NeedMoreData};
    std::size_t consumedBytes{};
    std::size_t frameCount{};
    std::size_t processedFrameBytes{};
    bool needsContinuation{false};
};

class PacketFramerViewPrototype {
public:
    explicit PacketFramerViewPrototype(protocol::PacketFramerOptions options);

    PacketFramerViewPrototype(const PacketFramerViewPrototype&) = delete;
    PacketFramerViewPrototype& operator=(const PacketFramerViewPrototype&) = delete;

    template <typename Visitor>
    FrameVisitResult visitFrames(
        std::span<const std::byte> input,
        Visitor&& visitor) {
        if (visitActive_) {
            return {.status = FrameVisitStatus::ReentrantVisitRejected};
        }
        if (faulted_) {
            return {.status = FrameVisitStatus::Faulted};
        }
        if (input.size() > options_.maxBufferedBytes) {
            faulted_ = true;
            return {.status = FrameVisitStatus::BufferLimitExceeded};
        }

        struct VisitGuard {
            bool& active;
            explicit VisitGuard(bool& value) noexcept : active(value) { active = true; }
            ~VisitGuard() { active = false; }
        } guard(visitActive_);

        std::size_t plannedBytes = 0;
        std::size_t plannedFrames = 0;
        std::size_t plannedFrameBytes = 0;
        bool budgetExhausted = false;

        while (input.size() - plannedBytes >= protocol::PacketFramer::kLengthBytes) {
            const auto payloadBytes = payloadLength(input, plannedBytes);
            if (payloadBytes > options_.maxPayloadBytes) {
                faulted_ = true;
                return {.status = FrameVisitStatus::FrameTooLarge};
            }

            const auto frameBytes = protocol::PacketFramer::kLengthBytes + payloadBytes;
            if (input.size() - plannedBytes < frameBytes) break;
            if (plannedFrames >= options_.maxFramesPerPush ||
                frameBytes > options_.maxFrameBytesPerPush - plannedFrameBytes) {
                budgetExhausted = true;
                break;
            }

            plannedBytes += frameBytes;
            plannedFrameBytes += frameBytes;
            ++plannedFrames;
        }

        std::size_t consumedBytes = 0;
        std::size_t visitedFrames = 0;
        std::size_t visitedFrameBytes = 0;
        while (visitedFrames < plannedFrames) {
            const auto payloadBytes = payloadLength(input, consumedBytes);
            const auto frameBytes = protocol::PacketFramer::kLengthBytes + payloadBytes;
            const auto payload = input.subspan(
                consumedBytes + protocol::PacketFramer::kLengthBytes,
                payloadBytes);
            try {
                std::invoke(visitor, PacketView(payload));
            } catch (...) {
                return {
                    .status = FrameVisitStatus::VisitorException,
                    .consumedBytes = consumedBytes,
                    .frameCount = visitedFrames,
                    .processedFrameBytes = visitedFrameBytes,
                    .needsContinuation = false,
                };
            }
            consumedBytes += frameBytes;
            visitedFrameBytes += frameBytes;
            ++visitedFrames;
        }

        return {
            .status = budgetExhausted
                ? FrameVisitStatus::BudgetExhausted
                : (visitedFrames == 0
                       ? FrameVisitStatus::NeedMoreData
                       : FrameVisitStatus::FramesVisited),
            .consumedBytes = consumedBytes,
            .frameCount = visitedFrames,
            .processedFrameBytes = visitedFrameBytes,
            .needsContinuation = budgetExhausted,
        };
    }

    bool reset() noexcept;
    bool faulted() const noexcept { return faulted_; }
    const protocol::PacketFramerOptions& options() const noexcept { return options_; }

private:
    static std::size_t payloadLength(
        std::span<const std::byte> input,
        std::size_t offset) noexcept;

    protocol::PacketFramerOptions options_;
    bool visitActive_{false};
    bool faulted_{false};
};

}  // namespace gamenet::experimental::hp1
