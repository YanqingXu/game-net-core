// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Implements intents/modules/udp.intent.md UDP-C2 and
// rules/{thread_affinity,ownership}_rules.md.
#include "gamenet/udp/UdpPeer.h"
#include "../core/net/detail/EventLoopLifecycleRegistry.h"
#include "UdpPeerTestAccess.h"
#include "gamenet/core/net/EventLoop.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::udp {
using CloseReason = UdpPeerCloseReason;
namespace {
std::atomic<std::uint64_t> sessionSequence{};
std::uint64_t nextSession() {
    auto n = sessionSequence.load(std::memory_order_relaxed);
    while (n != std::numeric_limits<std::uint64_t>::max())
        if (sessionSequence.compare_exchange_weak(n, n + 1, std::memory_order_relaxed))
            return n + 1;
    return 0;
}
std::string addressKey(const net::InetAddress& a) {
    std::string key;
    const auto append = [&](const auto& value) {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    const auto family = a.family();
    append(family);
    if (a.isIpv6()) {
        const auto& v = a.getSockAddrInet6();
        append(v.sin6_addr);
        append(v.sin6_port);
        append(v.sin6_scope_id);
    } else {
        const auto& v = a.getSockAddrInet();
        append(v.sin_addr);
        append(v.sin_port);
    }
    return key;
}
} // namespace
void UdpPeerOptions::validate() const {
    socket.validate();
    if (!maxPeers || maxPeers > 4096 || !maxPendingDatagramsPerPeer ||
        maxPendingDatagramsPerPeer > socket.maxPendingDatagrams ||
        maxPendingBytesPerPeer < socket.maxDatagramBytes ||
        maxPendingBytesPerPeer > socket.maxPendingBytes || !maxPeersPerTurn ||
        maxPeersPerTurn > 4096 || !maxCompletionsPerTurn || maxCompletionsPerTurn > 65536 ||
        idleTimeout < std::chrono::milliseconds(1) || idleTimeout > std::chrono::hours(24))
        throw std::invalid_argument("invalid UDP peer budgets/idle timeout");
}
namespace detail {
// Handles retain only this mailbox, never a server/loop lease. A lifecycle
// signal may coalesce with a partial scan; the work request must survive it.
struct UdpPeerScanRequest {
    std::atomic<bool> pending{false};
};
struct UdpPeerGate {
    const UdpPeerIdentity identity;
    const net::InetAddress address;
    const net::EventLoopExecutor executor;
    const net::EventLoopLifecycleSource source;
    const std::shared_ptr<UdpPeerScanRequest> scanRequest;
    std::mutex mutex;
    std::atomic<bool> open{true};
    CloseReason reason{CloseReason::Normal};
    UdpPeerGate(UdpPeerIdentity id, net::InetAddress a, net::EventLoopExecutor e,
                net::EventLoopLifecycleSource s, std::shared_ptr<UdpPeerScanRequest> scan)
        : identity(id), address(a), executor(e), source(s), scanRequest(std::move(scan)) {}
    bool seal(CloseReason r) {
        std::lock_guard lock(mutex);
        if (!open.load(std::memory_order_relaxed))
            return false;
        reason = r;
        open.store(false, std::memory_order_release);
        scanRequest->pending.store(true, std::memory_order_release);
        return true;
    }
};
struct UdpPeerState : std::enable_shared_from_this<UdpPeerState> {
    using Clock = std::chrono::steady_clock;
    struct Packet {
        std::uint64_t id;
        std::string bytes;
    };
    struct Record {
        std::shared_ptr<UdpPeerGate> gate;
        std::shared_ptr<UdpPeer> handle;
        std::deque<Packet> queue;
        std::size_t pendingBytes{};
        Clock::time_point activity{Clock::now()};
        std::string key;
    };
    struct Receipt {
        UdpPeerIdentity peer;
        std::uint64_t localId{}, nativeId{};
    };
    net::EventLoop* const loop;
    const net::EventLoopExecutor executor;
    const UdpPeerOptions options;
    UdpPeerCallbacks callbacks;
    std::shared_ptr<UdpSocket> socket;
    std::vector<std::shared_ptr<Record>> slots;
    std::vector<std::uint64_t> generations;
    std::vector<std::size_t> ready;
    std::map<std::string, std::size_t> addresses;
    std::optional<Receipt> inFlight;
    std::shared_ptr<UdpPeerState> activeLease;
    net::TimerId timer;
    net::EventLoopLifecycleSource source;
    const std::shared_ptr<UdpPeerScanRequest> scanRequest{std::make_shared<UdpPeerScanRequest>()};
    std::mutex stopMutex;
    UdpPhase phase{UdpPhase::Created};
    std::atomic<bool> stopping{false};
    bool nativeStopped{}, posted{}, scanAgain{};
    std::size_t scanCursor{}, scanRemaining{}, idleCursor{};
    std::uint64_t nextId{};
    UdpPeerSnapshot stats;
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
    std::function<void()> beforeSocketStartHook, afterScanHook;
    static void runHook(std::function<void()>& hook) {
        if (auto callback = std::exchange(hook, {}))
            callback();
    }
#endif

    UdpPeerState(net::EventLoop* owner, UdpPeerCallbacks cb, UdpPeerOptions o)
        : loop(owner), executor(owner->executor()), options(o), callbacks(std::move(cb)),
          slots(o.maxPeers), generations(o.maxPeers) {
        ready.reserve(o.maxPeers);
    }
    std::shared_ptr<Record> find(UdpPeerIdentity id) const {
        if (id.slot >= slots.size())
            return {};
        auto r = slots[id.slot];
        return r && r->gate->identity == id ? r : nullptr;
    }
    template <class F> void invoke(F&& f) noexcept {
        try {
            f();
        } catch (...) {
            ++stats.callbackExceptions;
            (void)requestStop();
        }
    }
    UdpPeerSnapshot snapshot() {
        loop->assertInLoopThread();
        auto s = stats;
        {
            std::lock_guard lock(stopMutex);
            s.phase = phase;
        }
        s.scheduledPosts = posted ? 1 : 0;
        s.socket = socket->snapshot();
        return s;
    }
    UdpResult start() {
        if (!executor.isInOwnerThread())
            return UdpResult::WrongThread;
        {
            std::lock_guard lock(stopMutex);
            if (phase != UdpPhase::Created)
                return phase == UdpPhase::Running ? UdpResult::AlreadyStarted : UdpResult::Stopped;
            if (loop->phase() != net::EventLoopPhase::Running)
                return UdpResult::OwnerUnavailable;
            phase = UdpPhase::Starting;
        }
        auto self = shared_from_this();
        const std::weak_ptr<UdpPeerState> weak = self;
        try {
            source =
                net::detail::EventLoopLifecycleRegistry::attachQuiesceParticipant(*loop, [weak] {
                    if (auto s = weak.lock())
                        s->visit();
                });
            activeLease = self;
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
            runHook(beforeSocketStartHook);
#endif
            const auto result = socket->start();
            if (result != UdpResult::Accepted) {
                (void)requestStop();
                (void)source.signal();
                return result;
            }
            const auto interval =
                std::max(std::chrono::milliseconds(1),
                         std::min(options.idleTimeout / 2, std::chrono::milliseconds(100)));
            const auto scheduled = loop->tryRunEvery(interval, [weak] {
                if (auto s = weak.lock())
                    s->idle(Clock::now());
            });
            if (scheduled.result != net::PostResult::Accepted) {
                (void)requestStop();
                (void)source.signal();
                return UdpResult::OwnerUnavailable;
            }
            timer = scheduled.timerId;
            {
                std::lock_guard lock(stopMutex);
                phase = UdpPhase::Running;
            }
            (void)source.signal();
            return UdpResult::Accepted;
        } catch (const std::bad_alloc&) {
            failStart();
            return UdpResult::NoMemory;
        } catch (...) {
            failStart();
            return UdpResult::OwnerUnavailable;
        }
    }
    void failStart() {
        if (activeLease) {
            (void)requestStop();
            (void)source.signal();
            return;
        }
        std::lock_guard lock(stopMutex);
        stopping.store(true, std::memory_order_release);
        (void)socket->stop();
        phase = UdpPhase::Stopped;
    }
    UdpResult requestStop() noexcept {
        std::lock_guard lock(stopMutex);
        if (phase == UdpPhase::Stopped)
            return UdpResult::Stopped;
        if (!stopping.exchange(true, std::memory_order_acq_rel))
            scanRequest->pending.store(true, std::memory_order_release);
        if (phase == UdpPhase::Created) {
            (void)socket->stop();
            phase = UdpPhase::Stopped;
            return UdpResult::Stopped;
        }
        if (phase == UdpPhase::Starting)
            return UdpResult::Accepted;
        return source.signal() == net::PostResult::OwnerUnavailable ? UdpResult::OwnerUnavailable
                                                                    : UdpResult::Accepted;
    }
    UdpPeerAdmission open(const net::InetAddress& address) {
        if (!executor.isInOwnerThread())
            return {UdpResult::WrongThread, {}};
        std::lock_guard serviceLock(stopMutex);
        if (stopping.load(std::memory_order_acquire) || phase != UdpPhase::Running)
            return {UdpResult::Stopped, {}};
        if (address.family() != socket->localAddress().family() || !address.port())
            return {UdpResult::InvalidArgument, {}};
        try {
            auto key = addressKey(address);
            if (auto it = addresses.find(key); it != addresses.end()) {
                if (slots[it->second]->gate->open.load(std::memory_order_acquire))
                    return {UdpResult::InvalidArgument, {}};
                addresses.erase(it); // sealed old generation still owns its obligations
            }
            std::size_t slot = 0;
            for (; slot < slots.size(); ++slot)
                if (!slots[slot] && generations[slot] != std::numeric_limits<std::uint64_t>::max())
                    break;
            if (slot == slots.size())
                return {UdpResult::Overloaded, {}};
            const auto id = nextSession();
            if (!id)
                return {UdpResult::Overloaded, {}};
            const UdpPeerIdentity identity{id, slot, generations[slot] + 1};
            auto r = std::make_shared<Record>();
            r->gate = std::make_shared<UdpPeerGate>(identity, address, executor, source, scanRequest);
            r->handle = std::shared_ptr<UdpPeer>(new UdpPeer(weak_from_this(), r->gate));
            r->key = std::move(key);
            addresses.emplace(r->key, slot);
            generations[slot] = identity.generation;
            slots[slot] = r;
            ++stats.peers;
            ++stats.opened;
            return {UdpResult::Accepted, r->handle};
        } catch (const std::bad_alloc&) {
            return {UdpResult::NoMemory, {}};
        }
    }
    bool schedule() {
        if (posted || inFlight || ready.empty())
            return true;
        const auto weak = weak_from_this();
        const auto result = executor.post([weak] {
            if (auto s = weak.lock()) {
                s->posted = false;
                s->pulse();
            }
        });
        if (result != net::PostResult::Accepted)
            return false;
        posted = true;
        return true;
    }
    void enqueueReady(std::size_t slot) {
        if (std::find(ready.begin(), ready.end(), slot) == ready.end())
            ready.push_back(slot);
    }
    UdpSendAdmission send(UdpPeerIdentity id, std::string_view bytes) {
        auto r = find(id);
        if (!r)
            return {UdpResult::Stopped, 0};
        std::lock_guard serviceLock(stopMutex);
        std::lock_guard lock(r->gate->mutex);
        if (!r->gate->open.load(std::memory_order_relaxed) ||
            stopping.load(std::memory_order_acquire))
            return {UdpResult::Stopped, 0};
        if (bytes.size() > options.socket.maxDatagramBytes)
            return {UdpResult::InvalidArgument, 0};
        if (r->queue.size() >= options.maxPendingDatagramsPerPeer ||
            bytes.size() > options.maxPendingBytesPerPeer - r->pendingBytes ||
            stats.pendingDatagrams >= options.socket.maxPendingDatagrams ||
            bytes.size() > options.socket.maxPendingBytes - stats.pendingBytes ||
            nextId == std::numeric_limits<std::uint64_t>::max())
            return {UdpResult::Overloaded, 0};
        try {
            r->queue.push_back({nextId + 1, std::string(bytes)});
        } catch (const std::bad_alloc&) {
            return {UdpResult::NoMemory, 0};
        }
        if (!inFlight || inFlight->peer != id)
            enqueueReady(id.slot);
        if (!schedule()) {
            r->queue.pop_back();
            if (r->queue.empty())
                std::erase(ready, id.slot);
            return {UdpResult::Overloaded, 0};
        }
        ++nextId;
        ++stats.acceptedDatagrams;
        stats.acceptedBytes += bytes.size();
        ++stats.pendingDatagrams;
        stats.pendingBytes += bytes.size();
        r->pendingBytes += bytes.size();
        r->activity = Clock::now();
        return {UdpResult::Accepted, nextId};
    }
    void pulse() {
        auto lease = shared_from_this();
        if (stopping.load(std::memory_order_acquire)) {
            (void)source.signal();
            return;
        }
        if (inFlight || ready.empty())
            return;
        const auto slot = ready.front();
        ready.erase(ready.begin());
        auto r = slots[slot];
        if (!r || !r->gate->open.load(std::memory_order_acquire)) {
            (void)source.signal();
            if (!schedule())
                (void)requestStop();
            return;
        }
        const auto admission = socket->sendTo(r->gate->address, r->queue.front().bytes);
        if (admission.result != UdpResult::Accepted) {
            (void)requestStop();
            return;
        }
        inFlight = Receipt{r->gate->identity, r->queue.front().id, admission.id};
    }
    void terminal(const std::shared_ptr<Record>& r, UdpSendStatus status, int error = 0) {
        auto packet = std::move(r->queue.front());
        r->queue.pop_front();
        r->pendingBytes -= packet.bytes.size();
        --stats.pendingDatagrams;
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
        if (callbacks.sendComplete)
            invoke([&] {
                callbacks.sendComplete(r->gate->identity,
                                       {packet.id, status, packet.bytes.size(), error});
            });
    }
    void complete(UdpSendCompletion c) {
        auto lease = shared_from_this();
        if (!inFlight || inFlight->nativeId != c.id) {
            ++stats.ignoredCompletions;
            return;
        }
        const auto receipt = *inFlight;
        inFlight.reset();
        auto r = find(receipt.peer);
        if (!r || r->queue.empty() || r->queue.front().id != receipt.localId)
            std::terminate();
        terminal(r, c.status, c.nativeError);
        if (!r->queue.empty() && r->gate->open.load(std::memory_order_acquire))
            enqueueReady(receipt.peer.slot);
        if (!r->gate->open.load(std::memory_order_acquire) ||
            stopping.load(std::memory_order_acquire)) {
            // A prior slice may already have skipped this in-flight record.
            scanRequest->pending.store(true, std::memory_order_release);
            (void)source.signal();
        } else if (!schedule())
            (void)requestStop();
    }
    void receive(const net::InetAddress& address, std::string_view bytes, bool truncated) {
        auto lease = shared_from_this();
        if (stopping.load(std::memory_order_acquire))
            return;
        auto it = addresses.find(addressKey(address));
        if (it == addresses.end()) {
            ++stats.unknownDatagrams;
            return;
        }
        auto r = slots[it->second];
        {
            std::lock_guard serviceLock(stopMutex);
            std::lock_guard lock(r->gate->mutex);
            if (stopping.load(std::memory_order_relaxed) ||
                !r->gate->open.load(std::memory_order_relaxed))
                return;
        }
        ++stats.receivedDatagrams;
        r->activity = Clock::now();
        if (callbacks.datagram)
            invoke([&] { callbacks.datagram(r->handle, bytes, truncated); });
    }
    void idle(Clock::time_point now) {
        if (stopping.load(std::memory_order_acquire))
            return;
        for (std::size_t n = 0; n < std::min(options.maxPeersPerTurn, slots.size()); ++n) {
            const auto slot = idleCursor++ % slots.size();
            auto r = slots[slot];
            if (r && now >= r->activity + options.idleTimeout &&
                r->gate->seal(CloseReason::IdleTimeout)) {
                ++stats.idleClosed;
                (void)source.signal();
            }
        }
    }
    void visit() {
        auto lease = shared_from_this();
        {
            std::lock_guard lock(stopMutex);
            if (phase == UdpPhase::Stopped)
                return;
        }
        if (loop->phase() != net::EventLoopPhase::Running)
            (void)requestStop();
        if (stopping.load(std::memory_order_acquire)) {
            {
                std::lock_guard lock(stopMutex);
                phase = UdpPhase::Stopping;
            }
            if (timer.valid()) {
                (void)loop->tryCancel(timer);
                timer = {};
            }
            // C1's unstarted stop is synchronous and deliberately emits no
            // stopped callback. Do not wait for a native obligation it never
            // accepted when quit raced its start.
            if (socket->stop() == UdpResult::Stopped)
                nativeStopped = true;
        }
        // Restart coverage from the current cursor, preserving fairness while
        // revisiting every slot after newly published close/completion work.
        if (scanRequest->pending.exchange(false, std::memory_order_acq_rel) || !scanRemaining)
            scanRemaining = slots.size();
        std::size_t visited = 0, completed = 0;
        bool retry = false;
        while (scanRemaining && visited < options.maxPeersPerTurn) {
            const auto slot = scanCursor++ % slots.size();
            --scanRemaining;
            ++visited;
            auto r = slots[slot];
            if (!r)
                continue;
            if (stopping.load(std::memory_order_acquire))
                r->gate->seal(CloseReason::GoingAway);
            if (r->gate->open.load(std::memory_order_acquire))
                continue;
            if (auto it = addresses.find(r->key); it != addresses.end() && it->second == slot)
                addresses.erase(it);
            std::erase(ready, slot);
            if (inFlight && inFlight->peer == r->gate->identity)
                continue;
            while (!r->queue.empty() && completed < options.maxCompletionsPerTurn) {
                ++completed;
                terminal(r, UdpSendStatus::Cancelled);
            }
            if (!r->queue.empty()) {
                retry = true;
                continue;
            }
            slots[slot].reset();
            --stats.peers;
            ++stats.closed;
            if (callbacks.closed)
                invoke([&] { callbacks.closed(r->gate->identity, r->gate->reason); });
        }
        stats.maxPeersVisitedPerTurn = std::max(stats.maxPeersVisitedPerTurn, visited);
        stats.maxCompletionsPerTurn = std::max(stats.maxCompletionsPerTurn, completed);
#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
        runHook(afterScanHook);
#endif
        // Cancellation debt belongs to the whole bounded scan, not just the last
        // slice. A later slice visiting an empty/live slot cannot erase earlier
        // peers' remaining obligations.
        scanAgain = scanAgain || retry;
        if (scanRemaining || scanAgain || scanRequest->pending.load(std::memory_order_acquire)) {
            if (!scanRemaining)
                scanAgain = false;
            (void)source.signal();
        }
        if (!stopping.load(std::memory_order_acquire) && !schedule())
            (void)requestStop();
        if (stopping.load(std::memory_order_acquire) && nativeStopped && !stats.peers && !posted)
            finish();
    }
    void finish() {
        net::detail::EventLoopLifecycleRegistry::detach(*loop, source);
        {
            std::lock_guard lock(stopMutex);
            phase = UdpPhase::Stopped;
        }
        ready.clear();
        addresses.clear();
        auto cb = std::move(callbacks.stopped);
        callbacks = {};
        activeLease.reset();
        if (cb)
            invoke([&] { cb(snapshot()); });
    }
};
} // namespace detail

#ifdef GAMENET_INTERNAL_UDP_TEST_HOOKS
std::weak_ptr<void> detail::UdpPeerTestAccess::state(const UdpPeerServer& s) { return s.state_; }
void detail::UdpPeerTestAccess::beforeSocketStart(UdpPeerServer& s, std::function<void()> hook) {
    s.state_->loop->assertInLoopThread();
    s.state_->beforeSocketStartHook = std::move(hook);
}
void detail::UdpPeerTestAccess::afterScan(UdpPeerServer& s, std::function<void()> hook) {
    s.state_->loop->assertInLoopThread();
    s.state_->afterScanHook = std::move(hook);
}
void detail::UdpPeerTestAccess::receive(UdpPeerServer& s, const net::InetAddress& a,
                                        std::string_view b, bool t) {
    s.state_->loop->assertInLoopThread();
    s.state_->receive(a, b, t);
}
void detail::UdpPeerTestAccess::expire(UdpPeerServer& s) {
    s.state_->loop->assertInLoopThread();
    s.state_->idle(detail::UdpPeerState::Clock::now() + std::chrono::hours(48));
}
std::uint64_t detail::UdpPeerTestAccess::nativeId(const UdpPeerServer& s) {
    s.state_->loop->assertInLoopThread();
    return s.state_->inFlight ? s.state_->inFlight->nativeId : 0;
}
void detail::UdpPeerTestAccess::replay(UdpPeerServer& s, std::uint64_t id) {
    s.state_->loop->assertInLoopThread();
    s.state_->complete({id, UdpSendStatus::Sent, 0, 0});
}
std::pair<std::size_t, std::size_t> detail::UdpPeerTestAccess::pending(const UdpPeerServer& s,
                                                                       UdpPeerIdentity id) {
    s.state_->loop->assertInLoopThread();
    auto r = s.state_->find(id);
    return r ? std::pair{r->queue.size(), r->pendingBytes}
             : std::pair<std::size_t, std::size_t>{0, 0};
}
#endif

UdpPeer::UdpPeer(std::weak_ptr<detail::UdpPeerState> state,
                 std::shared_ptr<detail::UdpPeerGate> gate)
    : state_(std::move(state)), gate_(std::move(gate)) {}
UdpPeerIdentity UdpPeer::identity() const noexcept { return gate_->identity; }
net::InetAddress UdpPeer::address() const { return gate_->address; }
std::uint64_t UdpPeer::id() const noexcept { return gate_->identity.id; }
net::EventLoopExecutor UdpPeer::ownerExecutor() const noexcept { return gate_->executor; }
bool UdpPeer::isOpen() const noexcept {
    return gate_->open.load(std::memory_order_acquire) && gate_->executor.available();
}
UdpSendAdmission UdpPeer::sendDatagram(std::string_view bytes) {
    if (!gate_->open.load(std::memory_order_acquire))
        return {UdpResult::Stopped, 0};
    if (!gate_->executor.available())
        return {UdpResult::OwnerUnavailable, 0};
    if (!gate_->executor.isInOwnerThread())
        return {UdpResult::WrongThread, 0};
    if (auto s = state_.lock())
        return s->send(gate_->identity, bytes);
    return {UdpResult::Stopped, 0};
}
UdpResult UdpPeer::send(std::string_view bytes) { return sendDatagram(bytes).result; }
gamenet::DispatchResult UdpPeer::requestClose(CloseReason reason) noexcept {
    if (!gate_->seal(reason))
        return DispatchResult::EndpointClosed;
    const auto r = gate_->source.signal();
    if (r == net::PostResult::Accepted)
        return DispatchResult::Accepted;
    return r == net::PostResult::OwnerUnavailable ? DispatchResult::OwnerUnavailable
                                                  : DispatchResult::Shutdown;
}
UdpResult UdpPeer::close(CloseReason reason) {
    if (!gate_->open.load(std::memory_order_acquire))
        return UdpResult::Stopped;
    if (!gate_->executor.available())
        return UdpResult::OwnerUnavailable;
    if (!gate_->executor.isInOwnerThread())
        return UdpResult::WrongThread;
    return requestClose(reason) == DispatchResult::Accepted ? UdpResult::Accepted
                                                            : UdpResult::OwnerUnavailable;
}
std::shared_ptr<UdpPeerServer> UdpPeerServer::create(net::EventLoop* loop,
                                                     const net::InetAddress& local,
                                                     UdpPeerCallbacks cb, UdpPeerOptions o) {
    if (!loop)
        throw std::invalid_argument("UDP peer requires owner");
    loop->assertInLoopThread();
    o.validate();
    auto s = std::make_shared<detail::UdpPeerState>(loop, std::move(cb), o);
    const std::weak_ptr<detail::UdpPeerState> weak = s;
    auto native = o.socket;
    native.maxPendingDatagrams = 1;
    native.maxPendingBytes = native.maxDatagramBytes;
    s->socket = UdpSocket::create(loop, local,
                                  {.datagram =
                                       [weak](const auto& a, auto b, bool t) {
                                           if (auto p = weak.lock()) {
                                               try {
                                                   p->receive(a, b, t);
                                               } catch (...) {
                                                   (void)p->requestStop();
                                               }
                                           }
                                       },
                                   .sendComplete =
                                       [weak](auto c) {
                                           if (auto p = weak.lock())
                                               p->complete(c);
                                       },
                                   .stopped =
                                       [weak](auto) {
                                           if (auto p = weak.lock()) {
                                               p->nativeStopped = true;
                                               (void)p->requestStop();
                                               (void)p->source.signal();
                                           }
                                       }},
                                  native);
    return std::shared_ptr<UdpPeerServer>(new UdpPeerServer(std::move(s)));
}
UdpPeerServer::UdpPeerServer(std::shared_ptr<detail::UdpPeerState> state)
    : state_(std::move(state)) {}
UdpPeerServer::~UdpPeerServer() { (void)state_->requestStop(); }
UdpResult UdpPeerServer::start() { return state_->start(); }
UdpPeerAdmission UdpPeerServer::openPeer(const net::InetAddress& address) {
    return state_->open(address);
}
UdpResult UdpPeerServer::stop() noexcept { return state_->requestStop(); }
net::InetAddress UdpPeerServer::localAddress() const { return state_->socket->localAddress(); }
UdpPeerSnapshot UdpPeerServer::snapshot() const { return state_->snapshot(); }
} // namespace gamenet::udp
