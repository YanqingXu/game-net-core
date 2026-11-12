// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/udp/UdpSocket.h"
#include "UdpTestAccess.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "../core/net/detail/DatagramAccess.h"
#include "../core/net/detail/EventLoopLifecycleRegistry.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <cstring>
#include <cerrno>
#include <string>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace gamenet::udp {
namespace {
using namespace net;
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
std::atomic<bool> failPreparation{false};
std::atomic<int> failRead{0}, failWrite{0};
std::atomic<void (*)(void*)> datagramCommitHook{nullptr};
std::atomic<void*> datagramCommitContext{nullptr};
#endif
void socketCheck(bool okay, const char* operation) {
    if (!okay)
        throw std::system_error(sockets::lastError(), std::system_category(), operation);
}
} // namespace
void UdpOptions::validate() const {
    if (!maxDatagramBytes || maxDatagramBytes > 65507 || !maxPendingDatagrams ||
        maxPendingDatagrams > 65536 || maxPendingBytes < maxDatagramBytes ||
        maxPendingBytes > 1024ULL * 1024 * 1024 || !maxReceiveDatagramsPerBatch ||
        maxReceiveDatagramsPerBatch > 65536 || !maxSendDatagramsPerBatch ||
        maxSendDatagramsPerBatch > 65536 || maxReceiveBytesPerBatch < maxDatagramBytes ||
        maxSendBytesPerBatch < maxDatagramBytes)
        throw std::invalid_argument("invalid UDP packet/byte budgets");
}

struct UdpSocket::State : std::enable_shared_from_this<State> {
    struct Packet {
        std::uint64_t id;
        net::InetAddress peer;
        std::string bytes;
    };
    net::EventLoop* loop;
    const UdpOptions options;
    net::InetAddress local;
    net::SocketFd fd{net::kInvalidSocket};
    std::unique_ptr<net::Channel> channel;
    net::EventLoopLifecycleSource source;
    std::shared_ptr<State> activeLease;
    UdpCallbacks callbacks;
    mutable std::mutex mutex;
    std::deque<Packet> packets;
    UdpSnapshot stats;
    std::uint64_t nextId{};
    std::atomic<bool> stopRequested{false};
    std::vector<char> receiveStorage;
    bool waitingWritable{};
#ifdef _WIN32
    struct Native {
        net::IocpOperation operation{};
        State* owner{};
        sockaddr_storage peer{};
        int peerLength{sizeof(sockaddr_storage)};
        WSABUF buffer{};
        DWORD flags{};
        bool read{};
    } readNative, writeNative;
    bool readPending{}, writePending{};
#endif

    State(net::EventLoop* owner, const net::InetAddress& address, UdpCallbacks cb, UdpOptions opts)
        : loop(owner), options(opts), local(address), callbacks(std::move(cb)),
          receiveStorage(opts.maxDatagramBytes) {
        if (!loop)
            throw std::invalid_argument("UDP requires owner");
        loop->assertInLoopThread();
        options.validate();
        net::sockets::ensureInitialized();
        fd = net::sockets::createNonblockingDatagram(local.family());
        socketCheck(net::sockets::isValid(fd), "UDP socket");
        try {
            if (local.isIpv6()) {
                const int on = 1;
                socketCheck(::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                                         reinterpret_cast<const char*>(&on), sizeof(on)) == 0,
                            "UDP v6-only");
            }
            socketCheck(::bind(fd, local.getSockAddr(), local.getSockAddrLen()) == 0, "UDP bind");
            sockaddr_storage actual{};
            socketCheck(net::sockets::tryGetLocalAddr(fd, &actual), "UDP getsockname");
            local = net::InetAddress(actual);
        } catch (...) {
            net::sockets::close(fd);
            fd = net::kInvalidSocket;
            throw;
        }
#ifdef _WIN32
        for (auto* n : {&readNative, &writeNative}) {
            n->owner = this;
            n->read = n == &readNative;
            n->operation.kind =
                n->read ? net::IocpOperationKind::Read : net::IocpOperationKind::Write;
            n->operation.completionContext = n;
            n->operation.completionConsumer = +[](void* context, auto, bool current) {
                auto& native = *static_cast<Native*>(context);
                auto lease = native.owner->shared_from_this();
                lease->completeNative(native, current);
            };
        }
#endif
    }
    ~State() {
        if (net::sockets::isValid(fd))
            net::sockets::close(fd);
    }
    template <class F> void invoke(F&& f) noexcept {
        try {
            f();
        } catch (...) {
            ++stats.callbackExceptions;
            (void)requestStop();
        }
    }
    UdpSnapshot snapshot() const {
        loop->assertInLoopThread();
        std::lock_guard lock(mutex);
        auto s = stats;
        s.pendingDatagrams = packets.size();
        s.receiveStorageBytes = receiveStorage.size();
#ifdef _WIN32
        s.nativeOperations = std::size_t(readPending) + std::size_t(writePending);
#endif
        return s;
    }
    UdpResult start() {
        if (!loop->isInLoopThread())
            return UdpResult::WrongThread;
        {
            std::lock_guard lock(mutex);
            if (stats.phase != UdpPhase::Created)
                return stats.phase == UdpPhase::Running ? UdpResult::AlreadyStarted
                                                        : UdpResult::Stopped;
            if (stopRequested.load(std::memory_order_acquire))
                return UdpResult::Stopped;
            if (loop->phase() != net::EventLoopPhase::Running)
                return UdpResult::OwnerUnavailable;
            stats.phase = UdpPhase::Starting;
        }
        auto self = shared_from_this();
        const std::weak_ptr<State> weak = self;
        try {
            channel = std::make_unique<net::Channel>(loop, fd);
            channel->tie(self);
            channel->setReadCallback([weak](auto) {
                if (auto s = weak.lock())
                    s->receiveBatch();
            });
            channel->setWriteCallback([weak] {
                if (auto s = weak.lock()) {
                    s->waitingWritable = false;
                    s->channel->disableWriting();
                    (void)s->source.signal();
                }
            });
            channel->setErrorCallback([weak] {
                if (auto s = weak.lock())
                    s->receiveError(net::sockets::getSocketError(s->fd));
            });
            source =
                net::detail::EventLoopLifecycleRegistry::attachQuiesceParticipant(*loop, [weak] {
                    if (auto s = weak.lock())
                        s->visit();
                });
            activeLease = self;
            {
                std::lock_guard lock(mutex);
                stats.phase = UdpPhase::Running;
            }
            channel->enableReading();
#ifdef _WIN32
            submitReceive();
#endif
            (void)source.signal(); // includes stop racing initial owner registration
            return UdpResult::Accepted;
        } catch (const std::bad_alloc&) {
            fail(0);
            visit();
            return UdpResult::NoMemory;
        } catch (...) {
            fail(0);
            visit();
            return UdpResult::OwnerUnavailable;
        }
    }
    UdpSendAdmission send(const net::InetAddress& peer, std::string_view bytes) {
        if (peer.family() != local.family() || bytes.size() > options.maxDatagramBytes)
            return {UdpResult::InvalidArgument, 0};
        std::lock_guard lock(mutex);
        if (stats.phase != UdpPhase::Running || stopRequested.load(std::memory_order_acquire))
            return {UdpResult::Stopped, 0};
        if (packets.size() >= options.maxPendingDatagrams ||
            bytes.size() > options.maxPendingBytes - stats.pendingBytes)
            return {UdpResult::Overloaded, 0};
        if (nextId == std::numeric_limits<std::uint64_t>::max())
            return {UdpResult::Overloaded, 0};
        try {
            packets.push_back({nextId + 1, peer, std::string(bytes)});
        } catch (const std::bad_alloc&) {
            return {UdpResult::NoMemory, 0};
        }
        const auto notified = source.signal();
        if (notified != net::PostResult::Accepted) {
            packets.pop_back();
            return {UdpResult::OwnerUnavailable, 0};
        }
        ++nextId;
        ++stats.acceptedDatagrams;
        stats.acceptedBytes += bytes.size();
        stats.pendingBytes += bytes.size();
        return {UdpResult::Accepted, nextId};
    }
    UdpResult requestStop() noexcept {
        std::lock_guard lock(mutex);
        if (stats.phase == UdpPhase::Stopped)
            return UdpResult::Stopped;
        stopRequested.store(true, std::memory_order_release);
        if (stats.phase == UdpPhase::Starting)
            return UdpResult::Accepted; // start publishes a final lifecycle signal
        if (stats.phase == UdpPhase::Created) {
            // No owner registration/native I/O exists yet. Seal and release
            // raw resources without invoking an unstarted generation callback.
            if (net::sockets::isValid(fd)) {
                net::sockets::close(fd);
                fd = net::kInvalidSocket;
            }
            std::vector<char>().swap(receiveStorage);
            stats.phase = UdpPhase::Stopped;
            return UdpResult::Stopped;
        }
        const auto result = source.signal();
        // Quit itself has already committed the participant in Quiescing.
        return result == net::PostResult::OwnerUnavailable ? UdpResult::OwnerUnavailable
                                                           : UdpResult::Accepted;
    }
    void fail(int error) noexcept {
        stats.nativeError = error;
        (void)requestStop();
    }
    void receiveError(int error) noexcept {
        if (!error)
            return;
        ++stats.receiveErrors;
        stats.nativeError = error;
#ifdef _WIN32
        const bool perDatagram = error == WSAECONNRESET || error == WSAECONNREFUSED;
#else
        const bool perDatagram = error == ECONNRESET || error == ECONNREFUSED;
#endif
        if (perDatagram) {
            (void)source.signal();
            return;
        }
        fail(error);
    }
    void finishPacket(UdpSendStatus status, int error = 0) {
        Packet packet{0, net::InetAddress{}, {}};
        {
            std::lock_guard lock(mutex);
            if (packets.empty())
                std::terminate();
            packet = std::move(packets.front());
            packets.pop_front();
            stats.pendingBytes -= packet.bytes.size();
            switch (status) {
            case UdpSendStatus::Sent:
                ++stats.sentDatagrams;
                stats.sentBytes += packet.bytes.size();
                break;
            case UdpSendStatus::Cancelled:
                ++stats.cancelledDatagrams;
                stats.cancelledBytes += packet.bytes.size();
                break;
            case UdpSendStatus::Error:
                ++stats.failedDatagrams;
                stats.failedBytes += packet.bytes.size();
                break;
            }
        }
        if (callbacks.sendComplete)
            invoke(
                [&] { callbacks.sendComplete({packet.id, status, packet.bytes.size(), error}); });
    }
    void receive(const net::InetAddress& peer, std::size_t size, bool truncated) {
        ++stats.receivedDatagrams;
        stats.receivedBytes += size;
        if (truncated)
            ++stats.truncatedDatagrams;
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
        if (auto hook = datagramCommitHook.exchange(nullptr, std::memory_order_acquire))
            hook(datagramCommitContext.load(std::memory_order_relaxed));
#endif
        {
            // This is callback commitment, serialized with the stop seal.
            // Already committed owner frames may finish; no new frame crosses
            // a foreign stop that won the same admission lock.
            std::lock_guard lock(mutex);
            if (stopRequested.load(std::memory_order_acquire) || stats.phase != UdpPhase::Running)
                return;
        }
        if (callbacks.datagram)
            invoke([&] { callbacks.datagram(peer, {receiveStorage.data(), size}, truncated); });
    }
    void receiveBatch() {
#ifndef _WIN32
        if (stopRequested.load(std::memory_order_acquire))
            return;
        std::size_t count = 0, bytes = 0, attempts = 0;
        while (!stopRequested.load(std::memory_order_acquire) &&
               attempts++ < options.maxReceiveDatagramsPerBatch &&
               bytes + options.maxDatagramBytes <= options.maxReceiveBytesPerBatch) {
            sockaddr_storage peer{};
            iovec buffer{receiveStorage.data(), receiveStorage.size()};
            msghdr message{};
            message.msg_name = &peer;
            message.msg_namelen = sizeof(peer);
            message.msg_iov = &buffer;
            message.msg_iovlen = 1;
            ssize_t n;
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
            if (const auto error = failRead.exchange(0)) {
                net::sockets::setLastError(error);
                n = -1;
            } else
#endif
                n = ::recvmsg(fd, &message, 0);
            if (n < 0) {
                const auto error = net::sockets::lastError();
                if (net::sockets::isInterrupted(error))
                    continue;
                if (!net::sockets::isWouldBlock(error))
                    receiveError(error);
                break;
            }
            const auto size =
                std::min<std::size_t>(static_cast<std::size_t>(n), receiveStorage.size());
            ++count;
            bytes += size;
            receive(net::InetAddress(peer), size, (message.msg_flags & MSG_TRUNC) != 0);
        }
        ++stats.receiveBatches;
        stats.maxReceiveBatchDatagrams = std::max(stats.maxReceiveBatchDatagrams, count);
        stats.maxReceiveBatchBytes = std::max(stats.maxReceiveBatchBytes, bytes);
#endif
    }
    void sendBatch(bool cancelling) {
        std::size_t count = 0, bytes = 0;
        for (; count < options.maxSendDatagramsPerBatch; ++count) {
#ifdef _WIN32
            if (writePending)
                break;
#endif
            std::size_t size = 0;
            int error = 0;
            {
                std::lock_guard lock(mutex);
                if (packets.empty())
                    break;
                auto& packet = packets.front();
                size = packet.bytes.size();
                if (bytes + size > options.maxSendBytesPerBatch)
                    break;
                if (!cancelling) {
#ifdef _WIN32
                    // Submission/commit has no user callback and all queue
                    // access stays under the same bounded-admission lock.
                    error = submitSend(packet);
                    if (!error) {
                        bytes += size;
                        ++count;
                        break;
                    }
#else
                    const auto n = net::sockets::sendTo(fd, packet.bytes.data(), size,
                                                        packet.peer.getSockAddr(),
                                                        packet.peer.getSockAddrLen());
                    if (n < 0)
                        error = net::sockets::lastError();
                    else if (static_cast<std::size_t>(n) != size)
                        error = EMSGSIZE;
#endif
                }
            }
            if (error &&
                (net::sockets::isWouldBlock(error) || net::sockets::isInterrupted(error))) {
#ifndef _WIN32
                waitingWritable = true;
                channel->enableWriting();
#else
                fail(error);
#endif
                break;
            }
            bytes += size;
            finishPacket(cancelling ? UdpSendStatus::Cancelled
                         : error    ? UdpSendStatus::Error
                                    : UdpSendStatus::Sent,
                         error);
            if (!cancelling && stopRequested.load(std::memory_order_acquire)) {
                ++count;
                break;
            }
        }
        ++stats.sendBatches;
        stats.maxSendBatchDatagrams = std::max(stats.maxSendBatchDatagrams, count);
        stats.maxSendBatchBytes = std::max(stats.maxSendBatchBytes, bytes);
    }
    void beginStop() {
        {
            std::lock_guard lock(mutex);
            stats.phase = UdpPhase::Stopping;
            stopRequested.store(true, std::memory_order_release);
        }
#ifdef _WIN32
        if (readPending)
            net::detail::DatagramAccess::cancel(*loop, readNative.operation, fd);
        if (writePending)
            net::detail::DatagramAccess::cancel(*loop, writeNative.operation, fd);
#endif
        if (channel) {
            if (loop->hasChannel(channel.get())) {
                channel->disableAll();
                channel->remove();
            }
#ifdef _WIN32
            readNative.operation.channel = nullptr;
            writeNative.operation.channel = nullptr;
#endif
            net::detail::DatagramAccess::retire(*loop, std::move(channel));
        }
        if (net::sockets::isValid(fd)) {
            net::sockets::close(fd);
            fd = net::kInvalidSocket;
        }
    }
    void visit() {
        auto lease = shared_from_this();
        if (stats.phase == UdpPhase::Stopped)
            return;
        if (loop->phase() != net::EventLoopPhase::Running)
            (void)requestStop();
        if (stopRequested.load(std::memory_order_acquire)) {
            if (stats.phase != UdpPhase::Stopping)
                beginStop();
            sendBatch(true);
            bool pending;
            {
                std::lock_guard lock(mutex);
                pending = !packets.empty();
            }
#ifdef _WIN32
            pending = pending || readPending || writePending;
#endif
            if (pending) {
                (void)source.signal();
                return;
            }
            if (net::sockets::isValid(fd)) {
                net::sockets::close(fd);
                fd = net::kInvalidSocket;
            }
            std::vector<char>().swap(receiveStorage);
            net::detail::EventLoopLifecycleRegistry::detach(*loop, source);
            {
                std::lock_guard lock(mutex);
                source = {};
                stats.phase = UdpPhase::Stopped;
            }
            auto stopped = std::move(callbacks.stopped);
            callbacks = {};
            if (stopped)
                invoke([&] { stopped(snapshot()); });
            activeLease.reset();
            return;
        }
#ifdef _WIN32
        if (!readPending)
            submitReceive();
#endif
        if (!stopRequested.load(std::memory_order_acquire) && !waitingWritable)
            sendBatch(false);
        bool more;
        {
            std::lock_guard lock(mutex);
            more = !packets.empty();
        }
#ifdef _WIN32
        more = more && !writePending;
#endif
        if ((more && !waitingWritable) || stopRequested.load(std::memory_order_acquire))
            (void)source.signal();
    }
#ifdef _WIN32
    void prepare(Native& native) {
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
        if (failPreparation.exchange(false))
            throw std::bad_alloc();
#endif
        native.operation.overlapped = {};
        native.operation.channel = channel.get();
        net::detail::DatagramAccess::prepare(*loop, native.operation, shared_from_this());
    }
    void submitReceive() {
        auto& n = readNative;
        n.peer = {};
        n.peerLength = sizeof(n.peer);
        n.flags = 0;
        n.buffer = {static_cast<ULONG>(receiveStorage.size()), receiveStorage.data()};
        try {
            prepare(n);
        } catch (...) {
            fail(WSAENOBUFS);
            return;
        }
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
        if (const auto error = failRead.exchange(0)) {
            net::detail::DatagramAccess::reject(*loop, n.operation);
            receiveError(error);
            return;
        }
#endif
        const int result =
            ::WSARecvFrom(fd, &n.buffer, 1, nullptr, &n.flags, reinterpret_cast<sockaddr*>(&n.peer),
                          &n.peerLength, &n.operation.overlapped, nullptr);
        const int error = result == SOCKET_ERROR ? net::sockets::lastError() : 0;
        if (error && error != WSA_IO_PENDING) {
            net::detail::DatagramAccess::reject(*loop, n.operation);
            if (error == WSAEMSGSIZE) {
                recordNativeReceive(options.maxDatagramBytes, true);
                (void)source.signal();
            } else
                receiveError(error);
            return;
        }
        net::detail::DatagramAccess::commit(*loop, n.operation, shared_from_this());
        readPending = true;
    }
    int submitSend(Packet& packet) {
        auto& n = writeNative;
        n.peer = {};
        std::memcpy(&n.peer, packet.peer.getSockAddr(), packet.peer.getSockAddrLen());
        n.peerLength = packet.peer.getSockAddrLen();
        n.buffer = {static_cast<ULONG>(packet.bytes.size()), packet.bytes.data()};
        try {
            prepare(n);
        } catch (...) {
            return WSAENOBUFS;
        }
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
        if (const auto error = failWrite.exchange(0)) {
            net::detail::DatagramAccess::reject(*loop, n.operation);
            return error;
        }
#endif
        const int result =
            ::WSASendTo(fd, &n.buffer, 1, nullptr, 0, reinterpret_cast<const sockaddr*>(&n.peer),
                        n.peerLength, &n.operation.overlapped, nullptr);
        const int error = result == SOCKET_ERROR ? net::sockets::lastError() : 0;
        if (error && error != WSA_IO_PENDING) {
            net::detail::DatagramAccess::reject(*loop, n.operation);
            return error;
        }
        net::detail::DatagramAccess::commit(*loop, n.operation, shared_from_this());
        writePending = true;
        return 0;
    }
    void recordNativeReceive(std::size_t bytes, bool truncated) {
        ++stats.receiveBatches;
        stats.maxReceiveBatchDatagrams = std::max<std::size_t>(stats.maxReceiveBatchDatagrams, 1);
        stats.maxReceiveBatchBytes = std::max(stats.maxReceiveBatchBytes, bytes);
        receive(net::InetAddress(readNative.peer), bytes, truncated);
    }
    void completeNative(Native& n, bool current) {
        if (n.read)
            readPending = false;
        else
            writePending = false;
        n.buffer = {};
        const bool stopping = stopRequested.load(std::memory_order_acquire) || !current;
        if (n.read) {
            if (!stopping) {
                if (!n.operation.error || n.operation.error == WSAEMSGSIZE)
                    recordNativeReceive(n.operation.error == WSAEMSGSIZE
                                            ? options.maxDatagramBytes
                                            : std::min<std::size_t>(n.operation.bytesTransferred,
                                                                    options.maxDatagramBytes),
                                        n.operation.error == WSAEMSGSIZE);
                else
                    receiveError(static_cast<int>(n.operation.error));
            }
        } else {
            {
                std::lock_guard lock(mutex);
                if (!n.operation.error &&
                    n.operation.bytesTransferred != packets.front().bytes.size())
                    n.operation.error = WSAEMSGSIZE;
            }
            const auto status = stopping            ? UdpSendStatus::Cancelled
                                : n.operation.error ? UdpSendStatus::Error
                                                    : UdpSendStatus::Sent;
            finishPacket(status, static_cast<int>(n.operation.error));
        }
        if (!current)
            (void)requestStop();
        (void)source.signal();
    }
#endif
};

#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
void detail::UdpTestAccess::beforeDatagramCommit(void (*hook)(void*), void* context) {
    datagramCommitContext.store(context, std::memory_order_relaxed);
    datagramCommitHook.store(hook, std::memory_order_release);
}
void detail::UdpTestAccess::failNextPreparation() { failPreparation.store(true); }
void detail::UdpTestAccess::failNextRead(int error) { failRead.store(error); }
void detail::UdpTestAccess::failNextWrite(int error) { failWrite.store(error); }
#endif

std::shared_ptr<UdpSocket> UdpSocket::create(net::EventLoop* loop, const net::InetAddress& local,
                                             UdpCallbacks cb, UdpOptions options) {
    options.validate();
    return std::shared_ptr<UdpSocket>(
        new UdpSocket(std::make_shared<State>(loop, local, std::move(cb), options)));
}
UdpSocket::UdpSocket(std::shared_ptr<State> state) : state_(std::move(state)) {}
UdpSocket::~UdpSocket() { (void)state_->requestStop(); }
UdpResult UdpSocket::start() { return state_->start(); }
UdpSendAdmission UdpSocket::sendTo(const net::InetAddress& peer, std::string_view bytes) {
    return state_->send(peer, bytes);
}
UdpResult UdpSocket::stop() noexcept { return state_->requestStop(); }
net::InetAddress UdpSocket::localAddress() const {
    state_->loop->assertInLoopThread();
    return state_->local;
}
UdpSnapshot UdpSocket::snapshot() const { return state_->snapshot(); }
} // namespace gamenet::udp
