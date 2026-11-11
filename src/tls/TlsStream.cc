// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/tls/TlsStream.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gamenet::tls {
namespace {
struct BioDeleter { void operator()(BIO* value) const noexcept { BIO_free(value); } };
bool terminal(TlsPhase phase) { return phase == TlsPhase::Closed || phase == TlsPhase::Failed; }
void validate(const TlsOptions& o) {
    if (!o.bioCapacity || o.bioCapacity > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        !o.maxCipherSendChunkBytes || !o.maxCipherInputBytes || !o.maxPendingPlaintextBytes || !o.maxPendingWrites ||
        !o.maxStepsPerDrive || o.handshakeTimeout.count() <= 0 || o.shutdownTimeout.count() <= 0 ||
        o.retryDelay.count() <= 0 || o.handshakeTimeout > std::chrono::hours(24) ||
        o.shutdownTimeout > std::chrono::hours(24)) throw std::invalid_argument("invalid TLS bounds");
}
}  // namespace

struct TlsStream::Impl {
    std::weak_ptr<net::TcpConnection> connection;
    net::EventLoop* loop;
    net::EventLoopExecutor executor;
    std::shared_ptr<TlsContext> context;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
    std::unique_ptr<BIO, BioDeleter> network;
    TlsCallbacks callbacks;
    TlsOptions options;
    TlsSnapshot stats;
    std::weak_ptr<TlsStream> stream;
    net::Buffer cipherInput;
    std::deque<std::string> writes;
    std::size_t writeOffset{};
    std::array<char, 16384> readBuffer{};
    net::TimerId retryTimer, deadlineTimer;
    bool driving{}, readWantWrite{}, shutdownWantWrite{}, tcpShutdown{};
    bool transportDown{}, orderlyEof{}, eofContinuationQueued{};

    Impl(const net::TcpConnectionPtr& conn, std::shared_ptr<TlsContext> ctx,
        SSL_CTX* native, TlsCallbacks cb, const std::string& name, TlsOptions opts)
        : connection(conn), loop(conn->getLoop()), executor(loop->executor()),
          context(std::move(ctx)), callbacks(std::move(cb)), options(opts) {
        validate(opts);
        if (!callbacks.data || !callbacks.closed ||
            (context->role() == TlsContext::Role::Client && (name.empty() || name.size() > 253 ||
                name.find('\0') != std::string::npos)))
            throw std::invalid_argument("TLS requires data/closed callbacks and a client peer identity");
        ssl.reset(SSL_new(native));
        if (!ssl) throw std::runtime_error("SSL_new failed");
        BIO* internal{}; BIO* external{};
        if (BIO_new_bio_pair(&internal, opts.bioCapacity, &external, opts.bioCapacity) != 1)
            throw std::runtime_error("BIO pair creation failed");
        network.reset(external);
        SSL_set_bio(ssl.get(), internal, internal); // transfers one shared BIO ownership
        SSL_set_mode(ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE);
        if (context->role() == TlsContext::Role::Client) {
            auto* parameters = SSL_get0_param(ssl.get());
            SSL_set_hostflags(ssl.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
            if (X509_VERIFY_PARAM_set1_ip_asc(parameters, name.c_str()) != 1 &&
                (SSL_set1_host(ssl.get(), name.c_str()) != 1 ||
                 SSL_set_tlsext_host_name(ssl.get(), name.c_str()) != 1))
                throw std::invalid_argument("TLS peer identity setup failed");
            SSL_set_connect_state(ssl.get());
        } else SSL_set_accept_state(ssl.get());
    }
    bool owner() const noexcept { return loop->isInLoopThread(); }
    bool ended() const noexcept { return terminal(stats.phase); }
    void notifyFlags() {
        const auto flags = SSL_get_shutdown(ssl.get());
        stats.sentCloseNotify = (flags & SSL_SENT_SHUTDOWN) != 0;
        stats.receivedCloseNotify = (flags & SSL_RECEIVED_SHUTDOWN) != 0;
        stats.verifyResult = SSL_get_verify_result(ssl.get());
    }
    void finish(TlsError error, bool closeTcp) {
        if (ended()) return;
        notifyFlags();
        stats.terminal = error;
        stats.phase = error == TlsError::Graceful || error == TlsError::Aborted ? TlsPhase::Closed : TlsPhase::Failed;
        (void)loop->tryCancel(retryTimer); (void)loop->tryCancel(deadlineTimer);
        retryTimer = {}; deadlineTimer = {};
        stats.discardedPlaintextBytes += stats.pendingPlaintextBytes;
        writes.clear(); stats.pendingPlaintextBytes = 0; writeOffset = 0;
        cipherInput.retrieveAll();
        auto callback = std::move(callbacks.closed);
        if (closeTcp) if (auto conn = connection.lock()) conn->forceClose();
        if (callback) try { callback(error); } catch (...) { ++stats.callbackExceptions; }
    }
    void deadline(std::chrono::milliseconds timeout, TlsError reason) {
        (void)loop->tryCancel(deadlineTimer); deadlineTimer = {};
        const auto weak = stream;
        try {
            const auto result = loop->tryRunAfter(timeout, [weak, reason] {
                if (auto self = weak.lock()) self->impl_->finish(reason, true);
            });
            if (result.result == net::PostResult::Accepted) { deadlineTimer = result.timerId; return; }
        } catch (...) {}
        finish(TlsError::SchedulingRejected, true);
    }
    void retry() {
        if (ended() || retryTimer.valid()) return;
        const auto weak = stream;
        try {
            const auto result = loop->tryRunAfter(options.retryDelay, [weak] {
                if (auto self = weak.lock()) { self->impl_->retryTimer = {}; self->drive(); }
            });
            if (result.result == net::PostResult::Accepted) { retryTimer = result.timerId; return; }
        } catch (...) {}
        finish(TlsError::SchedulingRejected, true);
    }
    bool sslRetry(int error) {
        if (error == SSL_ERROR_WANT_READ) { ++stats.wantRead; return true; }
        if (error == SSL_ERROR_WANT_WRITE) { ++stats.wantWrite; return true; }
        notifyFlags();
        finish(stats.verifyResult != X509_V_OK ? TlsError::CertificateRejected :
            stats.phase == TlsPhase::Handshaking ? TlsError::HandshakeFailed : TlsError::ProtocolError, true);
        return false;
    }
    bool flush(bool& progressed) {
        if (transportDown) {
            if (BIO_ctrl_pending(network.get()) != 0) {
                finish(TlsError::Truncated, false); return false;
            }
            return true;
        }
        const auto conn = connection.lock();
        if (!conn || !conn->connected()) { finish(TlsError::TransportError, true); return false; }
        // Peek before TCP admission: rejected bytes stay in the fixed BIO.
        char* bytes{};
        const auto available = BIO_nread0(network.get(), &bytes);
        if (available <= 0) return true;
        const auto count = std::min<std::size_t>(available, options.maxCipherSendChunkBytes);
        const auto result = conn->trySend(bytes, count);
        if (result == net::TcpSendResult::Accepted) {
            if (ended()) return false;
            BIO_nread(network.get(), &bytes, static_cast<int>(count));
            progressed = true;
            return true;
        }
        switch (result) {
        case net::TcpSendResult::Overloaded:
        case net::TcpSendResult::LoopOverloaded:
        case net::TcpSendResult::ServerOverloaded:
        case net::TcpSendResult::GlobalOverloaded:
            ++stats.transportRejections; retry(); return false;
        default: finish(TlsError::TransportError, true); return false;
        }
    }
    void feed(bool& progressed) {
        const auto count = std::min<std::size_t>(BIO_ctrl_get_write_guarantee(network.get()), cipherInput.readableBytes());
        if (!count) return;
        std::size_t written{};
        if (BIO_write_ex(network.get(), cipherInput.peek(), count, &written) != 1) {
            finish(TlsError::ProtocolError, true); return;
        }
        cipherInput.retrieve(written); progressed |= written != 0;
    }
    void beginDrain() {
        if (stats.phase != TlsPhase::Open) return;
        stats.phase = TlsPhase::Draining;
        deadline(options.shutdownTimeout, TlsError::ShutdownTimeout);
    }
    void read(bool& progressed) {
        std::size_t size{};
        ERR_clear_error();
        const int result = SSL_read_ex(ssl.get(), readBuffer.data(), readBuffer.size(), &size);
        const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
        readWantWrite = error == SSL_ERROR_WANT_WRITE;
        if (result == 1) {
            progressed = true; stats.receivedPlaintextBytes += size;
            try { callbacks.data(std::string_view(readBuffer.data(), size)); }
            catch (...) { ++stats.callbackExceptions; finish(TlsError::CallbackException, true); }
        } else if (error == SSL_ERROR_ZERO_RETURN) {
            const bool wasReceived = stats.receivedCloseNotify;
            notifyFlags(); beginDrain(); progressed |= !wasReceived;
        } else (void)sslRetry(error);
    }
    void write(bool& progressed) {
        auto& bytes = writes.front();
        const auto count = std::min(readBuffer.size(), bytes.size() - writeOffset);
        std::size_t written{};
        ERR_clear_error();
        const int result = SSL_write_ex(ssl.get(), bytes.data() + writeOffset, count, &written);
        const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
        if (result != 1) { (void)sslRetry(error); return; }
        progressed = true; writeOffset += written;
        stats.pendingPlaintextBytes -= written; stats.encryptedPlaintextBytes += written;
        if (writeOffset == bytes.size()) { writes.pop_front(); writeOffset = 0; }
    }
    void closeStep(bool& progressed) {
        const auto previous = SSL_get_shutdown(ssl.get());
        ERR_clear_error();
        const int result = SSL_shutdown(ssl.get());
        const int error = result < 0 ? SSL_get_error(ssl.get(), result) : SSL_ERROR_NONE;
        shutdownWantWrite = error == SSL_ERROR_WANT_WRITE;
        if (result < 0 && !sslRetry(error)) return;
        notifyFlags();
        progressed |= previous != SSL_get_shutdown(ssl.get());
    }
    void drive() {
        if (ended() || stats.phase == TlsPhase::Created || driving || (tcpShutdown && !transportDown)) return;
        driving = true;
        try {
            std::size_t steps = 0;
            for (; steps < options.maxStepsPerDrive && !ended(); ++steps) {
                bool progressed = false;
                if (!flush(progressed)) break;
                feed(progressed);
                if (ended()) break;
                if (stats.phase == TlsPhase::Handshaking) {
                    ERR_clear_error();
                    const int result = SSL_do_handshake(ssl.get());
                    const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
                    if (result == 1) {
                        stats.phase = TlsPhase::Open; progressed = true;
                        (void)loop->tryCancel(deadlineTimer); deadlineTimer = {};
                        notifyFlags();
                        if (callbacks.ready) try { callbacks.ready(); }
                        catch (...) { ++stats.callbackExceptions; finish(TlsError::CallbackException, true); }
                    } else (void)sslRetry(error);
                } else if (readWantWrite) {
                    if (stats.readPaused) break;
                    read(progressed);
                } else if (!writes.empty()) {
                    if (transportDown) { finish(TlsError::Truncated, false); break; }
                    write(progressed); // same front bytes/length on every WANT retry
                } else {
                    if (!stats.readPaused && !shutdownWantWrite) read(progressed);
                    if (!ended() && stats.phase == TlsPhase::Draining && writes.empty()) {
                        stats.phase = TlsPhase::Closing; progressed = true;
                    }
                    // After sending our alert, drain peer application records
                    // with SSL_read until ZERO_RETURN before completing shutdown.
                    if (!ended() && stats.phase == TlsPhase::Closing &&
                        (!stats.sentCloseNotify || stats.receivedCloseNotify || shutdownWantWrite))
                        closeStep(progressed);
                }
                if (ended() || !flush(progressed)) break;
                notifyFlags();
                if (stats.phase == TlsPhase::Closing && stats.sentCloseNotify && stats.receivedCloseNotify &&
                    BIO_ctrl_pending(network.get()) == 0) {
                    if (transportDown) {
                        const bool exhausted = cipherInput.readableBytes() == 0 &&
                            BIO_ctrl_pending(SSL_get_rbio(ssl.get())) == 0;
                        finish(orderlyEof && exhausted ? TlsError::Graceful : TlsError::Truncated, false);
                        break;
                    }
                    tcpShutdown = true;
                    if (auto conn = connection.lock()) conn->shutdown();
                    else finish(TlsError::TransportError, false);
                    break;
                }
                if (!progressed) break;
            }
            if (!ended() && transportDown) {
                if (steps == options.maxStepsPerDrive && !stats.readPaused) {
                    // Core can report EOF before the TLS per-turn budget has
                    // decoded bytes already received. One accepted owner post
                    // leases the stream after the application's down hook.
                    if (!eofContinuationQueued) {
                        const auto self = stream.lock();
                        const auto posted = executor.post([self] {
                            self->impl_->eofContinuationQueued = false; self->drive();
                        });
                        if (posted == net::PostResult::Accepted) eofContinuationQueued = true;
                        else finish(TlsError::SchedulingRejected, false);
                    }
                } else finish(TlsError::Truncated, false);
            } else if (!ended() && !tcpShutdown && (steps == options.maxStepsPerDrive || shutdownWantWrite ||
                    (readWantWrite && !stats.readPaused))) retry();
        } catch (...) { finish(TlsError::ProtocolError, true); }
        driving = false;
    }
};

TlsStream::TlsStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::shared_ptr<TlsStream> TlsStream::create(const net::TcpConnectionPtr& connection,
    std::shared_ptr<TlsContext> context, TlsCallbacks callbacks, std::string peerName, TlsOptions options) {
    if (!connection || !context) throw std::invalid_argument("TLS requires TCP and context");
    connection->getLoop()->assertInLoopThread();
    auto* native = static_cast<SSL_CTX*>(context->nativeContext());
    auto result = std::shared_ptr<TlsStream>(new TlsStream(std::make_unique<Impl>(
        connection, std::move(context), native, std::move(callbacks), peerName, options)));
    result->impl_->stream = result;
    return result;
}
TlsStream::~TlsStream() {
    impl_->loop->assertInLoopThread();
    const bool unfinished = !impl_->ended();
    // Do not synchronously invoke TCP-down while std::any/context is releasing
    // its final stream reference. Explicit abort still force-closes immediately.
    impl_->finish(TlsError::Aborted, false);
    if (unfinished) if (auto connection = impl_->connection.lock()) connection->shutdown();
}
void TlsStream::drive() { impl_->drive(); }
net::EventLoopExecutor TlsStream::ownerExecutor() const noexcept { return impl_->executor; }
TlsOperation TlsStream::start() {
    auto& s = *impl_;
    if (!s.owner()) return TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (s.stats.phase != TlsPhase::Created) return s.ended() ? TlsOperation::Closed : TlsOperation::NotReady;
    const auto conn = s.connection.lock();
    if (!conn || !conn->connected()) { s.finish(TlsError::TransportError, true); return TlsOperation::Closed; }
    s.stats.phase = TlsPhase::Handshaking;
    s.deadline(s.options.handshakeTimeout, TlsError::HandshakeTimeout); s.drive();
    return s.ended() ? TlsOperation::Closed : TlsOperation::Accepted;
}
TlsOperation TlsStream::trySend(std::string_view bytes) {
    auto& s = *impl_;
    if (!s.owner()) return TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (s.transportDown || s.ended()) return TlsOperation::Closed;
    if (s.stats.phase != TlsPhase::Open) return TlsOperation::NotReady;
    if (bytes.empty()) return TlsOperation::Accepted;
    if (s.writes.size() >= s.options.maxPendingWrites || bytes.size() >
        s.options.maxPendingPlaintextBytes - s.stats.pendingPlaintextBytes) return TlsOperation::Overloaded;
    try { s.writes.emplace_back(bytes); } catch (...) { return TlsOperation::Overloaded; }
    s.stats.pendingPlaintextBytes += bytes.size(); s.stats.acceptedPlaintextBytes += bytes.size();
    s.drive(); return TlsOperation::Accepted;
}
TlsOperation TlsStream::pauseRead() {
    if (!impl_->owner()) return TlsOperation::WrongThread;
    if (impl_->ended()) return TlsOperation::Closed;
    impl_->stats.readPaused = true; return TlsOperation::Accepted;
}
TlsOperation TlsStream::resumeRead() {
    if (!impl_->owner()) return TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (impl_->ended()) return TlsOperation::Closed;
    impl_->stats.readPaused = false; impl_->drive(); return TlsOperation::Accepted;
}
TlsOperation TlsStream::shutdown() {
    if (!impl_->owner()) return TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (impl_->ended()) return TlsOperation::Closed;
    if (impl_->stats.phase == TlsPhase::Created || impl_->stats.phase == TlsPhase::Handshaking)
        impl_->finish(TlsError::Aborted, true);
    else { impl_->beginDrain(); impl_->drive(); }
    return TlsOperation::Accepted;
}
TlsOperation TlsStream::abort() {
    if (!impl_->owner()) return TlsOperation::WrongThread;
    const auto self = shared_from_this(); impl_->finish(TlsError::Aborted, true); return TlsOperation::Accepted;
}
TlsOperation TlsStream::onMessage(net::Buffer& bytes) {
    if (!impl_->owner()) return TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (impl_->driving) { ++impl_->stats.reentrantMessages; return TlsOperation::NotReady; }
    if (impl_->ended()) { bytes.retrieveAll(); return TlsOperation::Closed; }
    if (impl_->tcpShutdown && bytes.readableBytes()) {
        bytes.retrieveAll(); impl_->finish(TlsError::ProtocolError, true); return TlsOperation::Closed;
    }
    const auto buffered = impl_->cipherInput.readableBytes() + BIO_ctrl_pending(SSL_get_rbio(impl_->ssl.get()));
    if (buffered > impl_->options.maxCipherInputBytes ||
        bytes.readableBytes() > impl_->options.maxCipherInputBytes - buffered) {
        bytes.retrieveAll(); impl_->finish(TlsError::InputLimit, true); return TlsOperation::Overloaded;
    }
    try { impl_->cipherInput.append(std::string_view(bytes.peek(), bytes.readableBytes())); }
    catch (...) { bytes.retrieveAll(); impl_->finish(TlsError::InputLimit, true); return TlsOperation::Overloaded; }
    bytes.retrieveAll(); impl_->drive(); return TlsOperation::Accepted;
}
void TlsStream::onWritable() {
    impl_->loop->assertInLoopThread(); const auto self = shared_from_this(); impl_->drive();
}
void TlsStream::onDisconnect() {
    impl_->loop->assertInLoopThread(); const auto self = shared_from_this();
    auto& s = *impl_; if (s.ended()) return;
    s.notifyFlags();
    const auto conn = s.connection.lock(); const auto close = conn ? conn->closeInfo() : std::nullopt;
    s.orderlyEof = close && (close->reason == net::TcpConnectionCloseReason::PeerEof ||
        close->reason == net::TcpConnectionCloseReason::GracefulShutdown);
    s.transportDown = true;
    (void)s.loop->tryCancel(s.retryTimer); s.retryTimer = {};
    if (s.stats.phase == TlsPhase::Created) s.finish(TlsError::Truncated, false);
    else s.drive();
}
TlsSnapshot TlsStream::snapshot() const {
    impl_->loop->assertInLoopThread(); auto result = impl_->stats;
    result.pendingWrites = impl_->writes.size();
    if (!impl_->ended()) {
        result.bufferedCipherInputBytes = impl_->cipherInput.readableBytes() + BIO_ctrl_pending(SSL_get_rbio(impl_->ssl.get()));
        result.bufferedCipherOutputBytes = BIO_ctrl_pending(impl_->network.get());
    }
    return result;
}
}  // namespace gamenet::tls
