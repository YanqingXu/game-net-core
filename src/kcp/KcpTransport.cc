// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Implements KCP-C1 intent and the thread/ownership rules; no Core API changes.
#include "gamenet/kcp/KcpTransport.h"
#include "../core/net/detail/EventLoopLifecycleRegistry.h"
#include "KcpEngine.h"
#include "KcpTestAccess.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

namespace gamenet::kcp {
using Clock = std::chrono::steady_clock;
using Phase = udp::UdpPhase;
namespace detail {
struct KcpGate {
    const KcpIdentity identity;
    const net::InetAddress address;
    const net::EventLoopExecutor executor;
    const net::EventLoopLifecycleSource source;
    const std::shared_ptr<std::atomic<bool>> stopping;
    const std::shared_ptr<std::atomic<bool>> dirty;
    std::mutex mutex;
    std::atomic<bool> open{true};
    KcpCloseReason reason{KcpCloseReason::Normal};
    KcpGate(KcpIdentity i, net::InetAddress a, net::EventLoopExecutor e,
            net::EventLoopLifecycleSource s, std::shared_ptr<std::atomic<bool>> stop,
            std::shared_ptr<std::atomic<bool>> changed)
        : identity(i), address(a), executor(e), source(s), stopping(std::move(stop)),
          dirty(std::move(changed)) {}
    bool seal(KcpCloseReason r) noexcept {
        std::lock_guard lock(mutex);
        if (!open.load(std::memory_order_relaxed))
            return false;
        reason = r;
        open.store(false, std::memory_order_release);
        dirty->store(true, std::memory_order_release);
        return true;
    }
};
struct KcpState : std::enable_shared_from_this<KcpState> {
    struct Pending {
        std::uint64_t id;
        std::size_t bytes;
        std::uint32_t end;
        bool acked{};
        Clock::time_point deadline;
    };
    struct Record {
        KcpState* state{};
        std::shared_ptr<KcpGate> gate;
        std::shared_ptr<KcpSession> handle;
        std::shared_ptr<udp::UdpPeer> peer;
        std::unique_ptr<KcpEngine> engine;
        std::deque<Pending> pending;
        std::size_t pendingBytes{}, reserved{};
        bool wireClosed{};
        Clock::time_point activity{Clock::now()};
    };
    net::EventLoop* const loop;
    const net::EventLoopExecutor executor;
    const KcpOptions options;
    KcpCallbacks callbacks;
    std::shared_ptr<udp::UdpPeerServer> socket;
    std::vector<std::shared_ptr<Record>> slots;
    std::set<std::uint32_t> history;
    std::shared_ptr<KcpState> activeLease;
    const std::shared_ptr<std::atomic<bool>> stopping = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> dirty = std::make_shared<std::atomic<bool>>(false);
    net::EventLoopLifecycleSource source;
    net::TimerId timer;
    std::mutex stopMutex;
    Phase phase{Phase::Created};
    bool nativeStopped{}, posted{}, again{}, cleanupAgain{};
    std::size_t cursor{}, remaining{}, cleanupCursor{}, cleanupRemaining{};
    std::uint64_t nextId{};
    KcpSnapshot stats;
    std::chrono::milliseconds timeOffset{};
    Clock::time_point now() const { return Clock::now() + timeOffset; }
    KcpState(net::EventLoop* l, KcpCallbacks cb, KcpOptions o)
        : loop(l), executor(l->executor()), options(o), callbacks(std::move(cb)),
          slots(o.maxSessions) {}
    std::shared_ptr<Record> find(udp::UdpPeerIdentity id) const {
        for (const auto& r : slots)
            if (r && r->gate->identity.peer == id)
                return r;
        return {};
    }
    template <class F> void invoke(F&& f) noexcept {
        try {
            f();
        } catch (...) {
            ++stats.callbackExceptions;
            (void)requestStop();
        }
    }
    KcpSnapshot snapshot() {
        loop->assertInLoopThread();
        auto s = stats;
        {
            std::lock_guard lock(stopMutex);
            s.phase = phase;
        }
        s.convHistory = history.size();
        s.scheduledPosts = posted ? 1 : 0;
        s.udp = socket->snapshot();
        return s;
    }
    KcpResult start() {
        if (!executor.isInOwnerThread())
            return KcpResult::WrongThread;
        {
            std::lock_guard lock(stopMutex);
            if (phase != Phase::Created)
                return phase == Phase::Running ? KcpResult::AlreadyStarted : KcpResult::Stopped;
            if (loop->phase() != net::EventLoopPhase::Running)
                return KcpResult::OwnerUnavailable;
            phase = Phase::Starting;
        }
        const auto weak = weak_from_this();
        try {
            source =
                net::detail::EventLoopLifecycleRegistry::attachQuiesceParticipant(*loop, [weak] {
                    if (auto s = weak.lock())
                        s->cleanup();
                });
            activeLease = shared_from_this();
            const auto result = socket->start();
            if (result != KcpResult::Accepted) {
                failStart();
                return result;
            }
            auto scheduled = loop->tryRunEvery(options.interval, [weak] {
                if (auto s = weak.lock())
                    s->schedule();
            });
            if (scheduled.result != net::PostResult::Accepted) {
                failStart();
                return KcpResult::OwnerUnavailable;
            }
            timer = scheduled.timerId;
            {
                std::lock_guard lock(stopMutex);
                phase = Phase::Running;
            }
            (void)source.signal();
            return KcpResult::Accepted;
        } catch (...) {
            if (activeLease)
                failStart();
            else {
                std::lock_guard lock(stopMutex);
                phase = Phase::Stopped;
                stopping->store(true);
                (void)socket->stop();
            }
            return KcpResult::OwnerUnavailable;
        }
    }
    void failStart() {
        {
            std::lock_guard lock(stopMutex);
            phase = Phase::Running;
        }
        (void)requestStop();
    }
    KcpResult requestStop() noexcept {
        std::lock_guard lock(stopMutex);
        if (phase == Phase::Stopped)
            return KcpResult::Stopped;
        if (!stopping->exchange(true, std::memory_order_acq_rel))
            dirty->store(true, std::memory_order_release);
        if (phase == Phase::Created) {
            (void)socket->stop();
            phase = Phase::Stopped;
            return KcpResult::Stopped;
        }
        // Source is attached before Starting acquires an active lease. A concurrent
        // early stop is observed by start and by its initial lifecycle signal.
        if (phase == Phase::Starting)
            return KcpResult::Accepted;
        return source.signal() == net::PostResult::OwnerUnavailable ? KcpResult::OwnerUnavailable
                                                                    : KcpResult::Accepted;
    }
    static int output(void* context, std::string_view bytes) noexcept {
        auto& r = *static_cast<Record*>(context);
        try {
            if (r.peer->sendDatagram(bytes).result == KcpResult::Accepted)
                return 0;
        } catch (...) {
        }
        ++r.state->stats.outputDrops;
        return -1; // Normal KCP retransmission, no side queue.
    }
    KcpSessionAdmission open(const net::InetAddress& address, std::uint32_t conv) {
        if (!executor.isInOwnerThread())
            return {KcpResult::WrongThread, {}};
        std::lock_guard lock(stopMutex);
        if (phase != Phase::Running || stopping->load(std::memory_order_acquire))
            return {KcpResult::Stopped, {}};
        if (!conv || history.contains(conv))
            return {KcpResult::InvalidArgument, {}};
        const auto reserved = KcpEngine::storageBytes(options);
        if (stats.sessions >= options.maxSessions || history.size() >= options.maxSessionOpenings ||
            reserved > options.maxProtocolBytes - stats.protocolBytes)
            return {KcpResult::Overloaded, {}};
        std::shared_ptr<udp::UdpPeer> peer;
        bool inserted = false;
        try {
            auto r = std::make_shared<Record>();
            r->state = this;
            r->activity = now();
            r->engine = std::make_unique<KcpEngine>(conv, options, output, r.get());
            history.insert(conv);
            inserted = true;
            // A locally sealed KCP generation may be replaced immediately on
            // owner, while its old record still owns native/terminal debt.
            for (const auto& old : slots)
                if (old && !old->gate->open.load(std::memory_order_acquire))
                    (void)old->peer->requestClose(udp::UdpPeerCloseReason::Replaced);
            auto admission = socket->openPeer(address);
            if (admission.result != KcpResult::Accepted) {
                history.erase(conv);
                return {admission.result, {}};
            }
            peer = admission.peer;
            r->peer = peer;
            r->gate = std::make_shared<KcpGate>(KcpIdentity{peer->identity(), conv}, address,
                                                executor, source, stopping, dirty);
            r->handle = std::shared_ptr<KcpSession>(new KcpSession(weak_from_this(), r->gate));
            r->reserved = reserved;
            *std::find(slots.begin(), slots.end(), nullptr) = r;
            ++stats.opened;
            ++stats.sessions;
            stats.protocolBytes += reserved;
            stats.peakProtocolBytes = std::max(stats.peakProtocolBytes, stats.protocolBytes);
            return {KcpResult::Accepted, r->handle};
        } catch (const std::bad_alloc&) {
            if (inserted)
                history.erase(conv);
            if (peer)
                (void)peer->close(udp::UdpPeerCloseReason::Overloaded);
            return {KcpResult::NoMemory, {}};
        }
    }
    void seal(const std::shared_ptr<Record>& r, KcpCloseReason reason) {
        if (r->gate->seal(reason))
            (void)source.signal();
    }
    KcpSendAdmission send(KcpIdentity identity, std::string_view bytes) {
        auto r = find(identity.peer);
        if (!r || r->gate->identity != identity)
            return {};
        std::lock_guard serviceLock(stopMutex);
        std::lock_guard lock(r->gate->mutex);
        if (stopping->load(std::memory_order_acquire) ||
            !r->gate->open.load(std::memory_order_relaxed))
            return {};
        if (bytes.size() > options.maxMessageBytes)
            return {KcpResult::InvalidArgument, 0};
        if (r->pending.size() >= options.maxPendingMessagesPerSession ||
            bytes.size() > options.maxPendingBytesPerSession - r->pendingBytes ||
            stats.pendingMessages >= options.maxPendingMessages ||
            bytes.size() > options.maxPendingBytes - stats.pendingBytes ||
            nextId == std::numeric_limits<std::uint64_t>::max())
            return {KcpResult::Overloaded, 0};
        try {
            r->pending.push_back({nextId + 1, bytes.size(), 0, false, now() + options.sendTimeout});
        } catch (const std::bad_alloc&) {
            return {KcpResult::NoMemory, 0};
        }
        auto end = r->engine->send(bytes);
        if (!end) {
            r->pending.pop_back();
            if (r->engine->failed()) {
                r->gate->reason = KcpCloseReason::ResourceExhausted;
                r->gate->open.store(false, std::memory_order_release);
                dirty->store(true, std::memory_order_release);
                (void)source.signal();
                return {KcpResult::NoMemory, 0};
            }
            return {KcpResult::Overloaded, 0};
        }
        r->pending.back().end = *end;
        ++nextId;
        ++stats.accepted;
        ++stats.pendingMessages;
        stats.pendingBytes += bytes.size();
        r->pendingBytes += bytes.size();
        r->activity = now();
        // The periodic timer already owns update scheduling. No inline output or callback.
        return {KcpResult::Accepted, nextId};
    }
    void commitAcks(const std::shared_ptr<Record>& r) {
        for (auto& p : r->pending)
            if (!p.acked && r->engine->acknowledged(p.end))
                p.acked = true;
    }
    void receive(udp::UdpPeerIdentity identity, std::string_view bytes, bool truncated) {
        auto r = find(identity);
        if (!r)
            return;
        {
            std::lock_guard serviceLock(stopMutex);
            std::lock_guard lock(r->gate->mutex);
            if (stopping->load(std::memory_order_acquire) ||
                !r->gate->open.load(std::memory_order_relaxed))
                return;
            auto result = truncated ? InputResult::Invalid : r->engine->input(bytes);
            if (result == InputResult::Invalid)
                ++stats.invalidDatagrams;
            else if (result == InputResult::AckBudget)
                ++stats.ackBudgetDrops;
            else if (result == InputResult::Failed) {
                r->gate->reason = KcpCloseReason::ResourceExhausted;
                r->gate->open.store(false, std::memory_order_release);
                dirty->store(true, std::memory_order_release);
                (void)source.signal();
            } else {
                r->activity = now();
                commitAcks(r);
            }
        }
        schedule();
    }
    void schedule() {
        if (stopping->load(std::memory_order_acquire)) {
            (void)source.signal();
            return;
        }
        if (posted)
            return;
        if (!remaining)
            remaining = slots.size();
        const auto weak = weak_from_this();
        if (executor.post([weak] {
                if (auto s = weak.lock()) {
                    s->posted = false;
                    s->pulse();
                }
            }) == net::PostResult::Accepted)
            posted = true;
        else {
            for (auto& r : slots)
                if (r)
                    seal(r, KcpCloseReason::SchedulingRejected);
            (void)requestStop();
        }
    }
    void terminal(const std::shared_ptr<Record>& r, KcpSendStatus status) {
        auto p = r->pending.front();
        r->pending.pop_front();
        --stats.pendingMessages;
        stats.pendingBytes -= p.bytes;
        r->pendingBytes -= p.bytes;
        if (status == KcpSendStatus::Acknowledged)
            ++stats.acknowledged;
        else if (status == KcpSendStatus::Cancelled)
            ++stats.cancelled;
        else
            ++stats.failed;
        if (callbacks.sendComplete)
            invoke([&] { callbacks.sendComplete(r->gate->identity, {p.id, status, p.bytes}); });
    }
    void pulse() {
        auto lease = shared_from_this();
        if (stopping->load(std::memory_order_acquire)) {
            (void)source.signal();
            return;
        }
        const auto now = this->now();
        const auto millis = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        std::size_t visited = 0, total = 0;
        while (remaining && visited < options.maxSessionsPerTurn) {
            --remaining;
            ++visited;
            auto r = slots[cursor++ % slots.size()];
            if (!r)
                continue;
            {
                std::lock_guard serviceLock(stopMutex);
                std::lock_guard lock(r->gate->mutex);
                if (stopping->load(std::memory_order_relaxed) ||
                    !r->gate->open.load(std::memory_order_relaxed))
                    continue;
                if (!r->engine->update(millis)) {
                    r->gate->reason = KcpCloseReason::Timeout;
                    r->gate->open.store(false, std::memory_order_release);
                    dirty->store(true, std::memory_order_release);
                    (void)source.signal();
                    continue;
                }
                commitAcks(r);
            }
            std::size_t work = 0;
            while (work < options.maxMessagesPerSessionTurn && !r->pending.empty() &&
                   r->pending.front().acked) {
                ++work;
                terminal(r, KcpSendStatus::Acknowledged);
            }
            while (work < options.maxMessagesPerSessionTurn) {
                std::optional<std::string_view> message;
                {
                    std::lock_guard serviceLock(stopMutex);
                    std::lock_guard lock(r->gate->mutex);
                    if (stopping->load(std::memory_order_relaxed) ||
                        !r->gate->open.load(std::memory_order_relaxed))
                        break;
                    message = r->engine->receive();
                }
                if (!message)
                    break;
                ++work;
                ++stats.received;
                if (callbacks.message)
                    invoke([&] { callbacks.message(r->handle, *message); });
            }
            total += work;
            if (work == options.maxMessagesPerSessionTurn)
                again = true;
            if (r->gate->open.load(std::memory_order_acquire)) {
                if (r->engine->failed())
                    seal(r, KcpCloseReason::ProtocolError);
                else if (!r->pending.empty() && !r->pending.front().acked &&
                         now >= r->pending.front().deadline)
                    seal(r, KcpCloseReason::Timeout);
                else if (now >= r->activity + options.idleTimeout)
                    seal(r, KcpCloseReason::IdleTimeout);
            }
        }
        stats.maxSessionsVisitedPerTurn = std::max(stats.maxSessionsVisitedPerTurn, visited);
        stats.maxMessagesPerTurn = std::max(stats.maxMessagesPerTurn, total);
        if (remaining || again) {
            if (!remaining)
                again = false;
            schedule();
        }
        if (stopping->load(std::memory_order_acquire))
            (void)source.signal();
    }
    void peerClosed(udp::UdpPeerIdentity id) {
        auto r = find(id);
        if (!r)
            return;
        r->wireClosed = true;
        seal(r, KcpCloseReason::TransportError);
        dirty->store(true, std::memory_order_release);
        (void)source.signal();
    }
    void cleanup() {
        auto lease = shared_from_this();
        {
            std::lock_guard lock(stopMutex);
            if (phase == Phase::Stopped)
                return;
        }
        if (loop->phase() != net::EventLoopPhase::Running)
            (void)requestStop();
        if (stopping->load(std::memory_order_acquire)) {
            {
                std::lock_guard lock(stopMutex);
                phase = Phase::Stopping;
            }
            if (timer.valid()) {
                (void)loop->tryCancel(timer);
                timer = {};
            }
            (void)socket->stop();
        }
        if (dirty->exchange(false, std::memory_order_acq_rel))
            cleanupAgain = true;
        if (!cleanupRemaining)
            cleanupRemaining = slots.size();
        std::size_t visited = 0;
        while (cleanupRemaining && visited++ < options.maxSessionsPerTurn) {
            --cleanupRemaining;
            const auto i = cleanupCursor++ % slots.size();
            auto r = slots[i];
            if (!r)
                continue;
            if (stopping->load(std::memory_order_acquire))
                r->gate->seal(KcpCloseReason::GoingAway);
            if (r->gate->open.load(std::memory_order_acquire))
                continue;
            if (r->engine) {
                r->engine.reset();
                stats.protocolBytes -= r->reserved;
                r->reserved = 0;
            }
            (void)r->peer->requestClose(udp::UdpPeerCloseReason::Normal);
            const auto reason = r->gate->reason;
            const auto status =
                (reason == KcpCloseReason::Normal || reason == KcpCloseReason::GoingAway)
                    ? KcpSendStatus::Cancelled
                    : KcpSendStatus::Failed;
            for (std::size_t n = 0; n < options.maxMessagesPerSessionTurn && !r->pending.empty();
                 ++n)
                terminal(r, r->pending.front().acked ? KcpSendStatus::Acknowledged : status);
            if (!r->pending.empty()) {
                cleanupAgain = true;
                continue;
            }
            if (!r->wireClosed)
                continue;
            slots[i].reset();
            --stats.sessions;
            ++stats.closed;
            if (callbacks.closed)
                invoke([&] { callbacks.closed(r->gate->identity, reason); });
        }
        if (cleanupRemaining || cleanupAgain) {
            if (!cleanupRemaining)
                cleanupAgain = false;
            (void)source.signal();
        }
        if (stopping->load(std::memory_order_acquire) && nativeStopped && !stats.sessions &&
            !posted)
            finish();
    }
    void finish() {
        net::detail::EventLoopLifecycleRegistry::detach(*loop, source);
        {
            std::lock_guard lock(stopMutex);
            phase = Phase::Stopped;
        }
        auto cb = std::move(callbacks.stopped);
        callbacks = {};
        activeLease.reset();
        if (cb)
            invoke([&] { cb(snapshot()); });
    }
};
} // namespace detail
#ifdef GAMENET_INTERNAL_KCP_TEST_HOOKS
std::weak_ptr<void> detail::KcpTestAccess::state(const KcpTransport& s) { return s.state_; }
void detail::KcpTestAccess::input(KcpTransport& s, KcpIdentity id, std::string_view bytes,
                                  bool truncated) {
    s.state_->loop->assertInLoopThread();
    s.state_->receive(id.peer, bytes, truncated);
}
void detail::KcpTestAccess::advance(KcpTransport& s, std::chrono::milliseconds by) {
    s.state_->loop->assertInLoopThread();
    s.state_->timeOffset += by;
    s.state_->schedule();
}
void detail::KcpTestAccess::exhaust(KcpTransport& s, KcpIdentity id) {
    s.state_->loop->assertInLoopThread();
    if (auto r = s.state_->find(id.peer); r && r->engine)
        r->engine->exhaustNextAllocationForTesting();
}
udp::UdpSendAdmission detail::KcpTestAccess::wire(KcpTransport& s, KcpIdentity id,
                                                  std::string_view bytes) {
    s.state_->loop->assertInLoopThread();
    if (auto r = s.state_->find(id.peer))
        return r->peer->sendDatagram(bytes);
    return {};
}
#endif
KcpSession::KcpSession(std::weak_ptr<detail::KcpState> s, std::shared_ptr<detail::KcpGate> g)
    : state_(std::move(s)), gate_(std::move(g)) {}
KcpIdentity KcpSession::identity() const noexcept { return gate_->identity; }
net::InetAddress KcpSession::address() const { return gate_->address; }
net::EventLoopExecutor KcpSession::ownerExecutor() const noexcept { return gate_->executor; }
bool KcpSession::isOpen() const noexcept {
    return gate_->open.load(std::memory_order_acquire) &&
           !gate_->stopping->load(std::memory_order_acquire) && gate_->executor.available();
}
KcpSendAdmission KcpSession::send(std::string_view bytes) {
    if (!isOpen())
        return {};
    if (!gate_->executor.isInOwnerThread())
        return {KcpResult::WrongThread, 0};
    if (auto s = state_.lock())
        return s->send(gate_->identity, bytes);
    return {};
}
gamenet::DispatchResult KcpSession::requestClose(KcpCloseReason reason) noexcept {
    if (!gate_->seal(reason))
        return DispatchResult::EndpointClosed;
    return gamenet::dispatchResult(gate_->source.signal());
}
KcpResult KcpSession::close(KcpCloseReason reason) {
    if (!isOpen())
        return KcpResult::Stopped;
    if (!gate_->executor.isInOwnerThread())
        return KcpResult::WrongThread;
    return requestClose(reason) == DispatchResult::Accepted ? KcpResult::Accepted
                                                            : KcpResult::OwnerUnavailable;
}
std::shared_ptr<KcpTransport> KcpTransport::create(net::EventLoop* loop,
                                                   const net::InetAddress& local, KcpCallbacks cb,
                                                   KcpOptions o) {
    if (!loop)
        throw std::invalid_argument("KCP requires owner");
    loop->assertInLoopThread();
    o.validate();
    auto s = std::make_shared<detail::KcpState>(loop, std::move(cb), o);
    const auto weak = std::weak_ptr(s);
    s->socket =
        udp::UdpPeerServer::create(loop, local,
                                   {.datagram =
                                        [weak](auto peer, auto bytes, bool truncated) {
                                            if (auto p = weak.lock())
                                                p->receive(peer->identity(), bytes, truncated);
                                        },
                                    .closed =
                                        [weak](auto id, auto) {
                                            if (auto p = weak.lock())
                                                p->peerClosed(id);
                                        },
                                    .stopped =
                                        [weak](auto) {
                                            if (auto p = weak.lock()) {
                                                p->nativeStopped = true;
                                                (void)p->requestStop();
                                                (void)p->source.signal();
                                            }
                                        }},
                                   o.udp);
    return std::shared_ptr<KcpTransport>(new KcpTransport(std::move(s)));
}
KcpTransport::KcpTransport(std::shared_ptr<detail::KcpState> s) : state_(std::move(s)) {}
KcpTransport::~KcpTransport() { (void)state_->requestStop(); }
KcpResult KcpTransport::start() { return state_->start(); }
KcpSessionAdmission KcpTransport::openSession(const net::InetAddress& a, std::uint32_t conv) {
    return state_->open(a, conv);
}
KcpResult KcpTransport::stop() noexcept { return state_->requestStop(); }
net::InetAddress KcpTransport::localAddress() const { return state_->socket->localAddress(); }
KcpSnapshot KcpTransport::snapshot() const { return state_->snapshot(); }
} // namespace gamenet::kcp
