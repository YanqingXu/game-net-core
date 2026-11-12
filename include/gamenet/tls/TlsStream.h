// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

// TLS-C1: bounded nonblocking TLS over an existing owner-local TCP connection.
#include "gamenet/tls/TlsContext.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace gamenet::net { class Buffer; }
namespace gamenet::tls {
enum class TlsPhase { Created, Handshaking, Open, Draining, Closing, Closed, Failed };
enum class TlsError { None, Graceful, Aborted, HandshakeFailed, CertificateRejected,
    HandshakeTimeout, ShutdownTimeout, ProtocolError, Truncated, InputLimit,
    TransportError, CallbackException, SchedulingRejected };
enum class TlsOperation { Accepted, WrongThread, Closed, NotReady, Overloaded, InvalidArgument };
struct TlsOptions {
    std::size_t bioCapacity{32768};
    std::size_t maxCipherSendChunkBytes{4096};
    std::size_t maxCipherInputBytes{256 * 1024};
    std::size_t maxPendingPlaintextBytes{1024 * 1024};
    std::size_t maxPendingWrites{256};
    std::size_t maxStepsPerDrive{64};
    std::chrono::milliseconds handshakeTimeout{10000};
    std::chrono::milliseconds shutdownTimeout{5000};
    std::chrono::milliseconds retryDelay{5};
};
struct TlsSnapshot {
    TlsPhase phase{TlsPhase::Created};
    TlsError terminal{TlsError::None};
    std::size_t pendingPlaintextBytes{}, pendingWrites{}, bufferedCipherInputBytes{}, bufferedCipherOutputBytes{};
    std::uint64_t acceptedPlaintextBytes{}, encryptedPlaintextBytes{}, discardedPlaintextBytes{}, receivedPlaintextBytes{};
    std::uint64_t wantRead{}, wantWrite{}, transportRejections{}, callbackExceptions{}, reentrantMessages{};
    long verifyResult{};
    bool readPaused{}, sentCloseNotify{}, receivedCloseNotify{};
};
struct TlsCallbacks {
    std::function<void()> ready;
    // Borrows bytes only until callback returns. Copy before retaining/posting.
    std::function<void(std::string_view)> data;
    std::function<void(TlsError)> closed;
};
class TlsStream : public std::enable_shared_from_this<TlsStream> {
public:
    static std::shared_ptr<TlsStream> create(const net::TcpConnectionPtr& connection,
        std::shared_ptr<TlsContext> context, TlsCallbacks callbacks,
        std::string peerName = {}, TlsOptions options = {});
    ~TlsStream(); // owner-only, outstanding stream settles as Aborted
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    // Owner-only. Foreign mutations reject before changing state.
    TlsOperation start();
    // Copies before Accepted. No application data callback is delivered inline;
    // a terminal callback may still re-enter the admission call.
    TlsOperation trySend(std::string_view plaintext);
    TlsOperation pauseRead();
    TlsOperation resumeRead();
    TlsOperation shutdown();
    TlsOperation abort();
    TlsOperation onMessage(net::Buffer& ciphertext);
    void onWritable();
    void onDisconnect();
    TlsSnapshot snapshot() const;
    net::EventLoopExecutor ownerExecutor() const noexcept;
private:
    struct Impl;
    explicit TlsStream(std::unique_ptr<Impl> impl);
    void drive();
    std::unique_ptr<Impl> impl_;
};
}  // namespace gamenet::tls
