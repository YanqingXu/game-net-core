// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/core/net/InetAddress.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace gamenet::net {
class EventLoop;
}
namespace gamenet::udp {
namespace detail {
class UdpTestAccess;
}

struct UdpOptions {
    std::size_t maxDatagramBytes{1200};
    std::size_t maxPendingDatagrams{256};
    std::size_t maxPendingBytes{1024 * 1024};
    std::size_t maxReceiveDatagramsPerBatch{32};
    std::size_t maxReceiveBytesPerBatch{64 * 1024};
    std::size_t maxSendDatagramsPerBatch{32};
    std::size_t maxSendBytesPerBatch{64 * 1024};
    void validate() const;
};
enum class UdpResult {
    Accepted,
    Stopped,
    AlreadyStarted,
    WrongThread,
    Overloaded,
    InvalidArgument,
    OwnerUnavailable,
    NoMemory
};
enum class UdpSendStatus { Sent, Cancelled, Error };
enum class UdpPhase { Created, Starting, Running, Stopping, Stopped };
struct UdpSendAdmission {
    UdpResult result{UdpResult::Stopped};
    std::uint64_t id{};
};
struct UdpSendCompletion {
    std::uint64_t id{};
    UdpSendStatus status{UdpSendStatus::Error};
    std::size_t bytes{};
    int nativeError{};
};
struct UdpSnapshot {
    UdpPhase phase{UdpPhase::Created};
    std::uint64_t acceptedDatagrams{}, acceptedBytes{}, sentDatagrams{}, sentBytes{};
    std::uint64_t cancelledDatagrams{}, cancelledBytes{}, failedDatagrams{}, failedBytes{};
    std::size_t pendingDatagrams{}, pendingBytes{}, nativeOperations{}, receiveStorageBytes{};
    std::uint64_t receivedDatagrams{}, receivedBytes{}, truncatedDatagrams{}, receiveErrors{},
        callbackExceptions{};
    std::uint64_t receiveBatches{}, sendBatches{};
    std::size_t maxReceiveBatchDatagrams{}, maxReceiveBatchBytes{}, maxSendBatchDatagrams{},
        maxSendBatchBytes{};
    int nativeError{};
};
struct UdpCallbacks {
    // Borrowed peer and payload are valid only during this owner callback.
    // A truncated prefix is explicit; no original wire length is promised.
    std::function<void(const net::InetAddress&, std::string_view, bool truncated)> datagram;
    std::function<void(UdpSendCompletion)> sendComplete;
    std::function<void(UdpSnapshot)> stopped;
};

// One-shot owner-loop socket. Create/start/snapshot/localAddress are owner-only.
// sendTo/stop and wrapper destruction are thread-safe. Destroying an active
// wrapper requests stop; the outliving owner must run/drain before destruction.
// Callbacks run on owner, may reenter/send/stop/release, and are contained.
class UdpSocket final {
  public:
    static std::shared_ptr<UdpSocket> create(net::EventLoop* owner, const net::InetAddress& local,
                                             UdpCallbacks callbacks = {}, UdpOptions options = {});
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpResult start();
    // Accepted owns one copied packet until one completion. Sent reports local
    // OS completion, not peer delivery. Rejection has id=0 and no callback.
    UdpSendAdmission sendTo(const net::InetAddress& peer, std::string_view bytes);
    UdpResult stop() noexcept;
    net::InetAddress localAddress() const;
    UdpSnapshot snapshot() const;

  private:
    friend class detail::UdpTestAccess;
    struct State;
    explicit UdpSocket(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
} // namespace gamenet::udp
