// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "ReplayProfile.h"
#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/game_session/SessionManager.h"
#include "gamenet/kcp_transport/KcpTransportEndpoint.h"
#include "gamenet/udp/UdpSocket.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(NDEBUG) || !defined(GAMENET_GATEWAY_REPLAY_RELEASE)
#error This replay requires a Release build.
#endif

using namespace gamenet;
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
using Us = std::chrono::microseconds;
using Stamp = std::int64_t;
using Id = std::uint64_t;
namespace profile = gateway_replay;
constexpr Us epoch{profile::epochUs};
constexpr Us tick{profile::broadcastIntervalUs};
constexpr int burstSize = profile::pressureMessages;
constexpr int healthyMask = 1;
constexpr int slowMask = 2;
constexpr int both = healthyMask | slowMask;
constexpr int maxEpochs = std::max(profile::smokeEpochs, profile::measureEpochs);
constexpr std::uint32_t healthyConv = 101;
constexpr std::uint32_t firstSlowConv = 200;
constexpr auto maxBroadcasts = maxEpochs * profile::epochUs / profile::broadcastIntervalUs + 1;
constexpr auto healthyLogLimit = maxBroadcasts + profile::warmupBroadcasts + maxEpochs;
constexpr auto slowLogLimit = healthyLogLimit + maxEpochs * (profile::pressureMessages + 1LL);
static_assert(profile::broadcastRecipients == 2 && profile::slowRecipients == 1);
static_assert(maxEpochs < 255 && healthyLogLimit <= 2000 && slowLogLimit <= 20000);

void check(bool ok, std::string_view why) {
    if (!ok) {
        std::cerr << "Replay invariant failed: " << why << std::endl;
        std::abort();
    }
}

struct Timebase {
    const Clock::time_point origin{Clock::now()};
    Stamp now() const {
        return std::chrono::duration_cast<Us>(Clock::now() - origin).count();
    }
    void at(Stamp us) const {
        std::this_thread::sleep_until(origin + Us{us});
    }
};

template <class T> T await(std::future<T> &future) {
    check(future.wait_for(5s) == std::future_status::ready, "owner or close deadline");
    return future.get();
}

struct Owner {
    net::EventLoopThread thread;
    net::EventLoop *loop;
    net::EventLoopExecutor executor;

    explicit Owner(const char *label)
        : thread({}, label), loop(thread.startLoop()), executor(loop->executor()) {}

    template <class F> auto call(F fn) {
        check(!executor.isInOwnerThread(), "coordinator is the only waiter");
        auto task = std::make_shared<std::packaged_task<std::invoke_result_t<F>()>>(std::move(fn));
        auto done = task->get_future();
        check(executor.post([task] { (*task)(); }) == net::PostResult::Accepted, "owner post");
        return await(done);
    }
};

enum class Kind { Warmup, Fanout, Oversize, Pressure, Probe };

std::string_view name(Kind kind) {
    switch (kind) {
    case Kind::Warmup: return "warmup";
    case Kind::Fanout: return "fanout";
    case Kind::Oversize: return "oversize";
    case Kind::Pressure: return "pressure";
    case Kind::Probe: return "probe";
    }
    std::abort();
}

std::string payload(Kind kind, Id id) {
    const std::string seq = std::to_string(id) + " ";
    switch (kind) {
    case Kind::Warmup: return "BROADCAST " + seq + std::string(profile::warmupBody);
    case Kind::Fanout: return "BROADCAST " + seq + std::string(profile::broadcastBody);
    case Kind::Oversize: return "BROADCAST " + seq + std::string(profile::oversizeBodyBytes, 'b');
    case Kind::Pressure: return "RESULT " + seq + std::string(profile::pressureBodyBytes, 's');
    case Kind::Probe: return "RESULT " + seq + std::string(profile::recoveryBody);
    }
    std::abort();
}

Id messageId(std::string_view bytes) {
    const auto first = bytes.find(' ');
    check(first != bytes.npos, "message prefix");
    const auto end = bytes.find(' ', first + 1);
    check(end != bytes.npos, "message id separator");
    Id id{};
    auto result = std::from_chars(bytes.data() + first + 1, bytes.data() + end, id);
    check(result.ec == std::errc{} && result.ptr == bytes.data() + end && id, "message id");
    return id;
}

Kind checkPayload(std::string_view bytes, Id id) {
    const auto body = bytes.substr(bytes.find(' ', bytes.find(' ') + 1) + 1);
    Kind kind = Kind::Oversize;
    if (body == profile::warmupBody) kind = Kind::Warmup;
    else if (body == profile::broadcastBody) kind = Kind::Fanout;
    else if (body == profile::recoveryBody) kind = Kind::Probe;
    else if (body.size() == profile::pressureBodyBytes) kind = Kind::Pressure;
    check(payload(kind, id) == bytes, "received bytes differ from published payload");
    return kind;
}

const char *admissionName(kcp::KcpResult result) {
    switch (result) {
    case kcp::KcpResult::Accepted: return "accepted";
    case kcp::KcpResult::Stopped: return "closed";
    case kcp::KcpResult::Overloaded: return "overloaded";
    case kcp::KcpResult::NoMemory: return "no_memory";
    case kcp::KcpResult::OwnerUnavailable: return "owner_unavailable";
    default: break;
    }
    check(false, "unexpected KCP send admission");
    std::abort();
}

const char *terminalName(kcp::KcpSendStatus status) {
    switch (status) {
    case kcp::KcpSendStatus::Acknowledged: return "acknowledged";
    case kcp::KcpSendStatus::Cancelled: return "cancelled";
    case kcp::KcpSendStatus::Failed: return "failed";
    }
    std::abort();
}

void checkSettled(const udp::UdpSnapshot &s) {
    check(s.phase == udp::UdpPhase::Stopped && !s.pendingDatagrams && !s.pendingBytes &&
          !s.nativeOperations && !s.receiveStorageBytes && !s.callbackExceptions &&
          s.acceptedDatagrams == s.sentDatagrams + s.cancelledDatagrams + s.failedDatagrams,
          "native UDP settlement");
}

void checkSettled(const kcp::KcpSnapshot &s) {
    check(s.phase == udp::UdpPhase::Stopped && s.opened == s.closed && !s.sessions &&
          !s.pendingMessages && !s.pendingBytes && !s.scheduledPosts && !s.protocolBytes &&
          !s.callbackExceptions && s.accepted == s.acknowledged + s.cancelled + s.failed &&
          s.udp.phase == udp::UdpPhase::Stopped && s.udp.opened == s.udp.closed &&
          !s.udp.peers && !s.udp.pendingDatagrams && !s.udp.pendingBytes &&
          !s.udp.scheduledPosts && !s.udp.callbackExceptions, "KCP and peer settlement");
    checkSettled(s.udp.socket);
}

struct Admission {
    Id seq, sendId;
    std::uint32_t conv;
    Stamp at;
    std::size_t bytes;
    kcp::KcpResult result;
};
struct Terminal {
    std::uint32_t conv;
    Stamp at;
    kcp::KcpSendCompletion completion;
};
struct Receive {
    Id seq;
    std::uint32_t conv;
    Kind kind;
    Stamp at;
    std::size_t bytes;
};

struct Side {
    Owner &owner;
    const Timebase &time;
    const std::size_t logLimit;
    std::shared_ptr<kcp::KcpTransport> transport;
    std::shared_ptr<kcp::KcpSession> session;
    std::promise<kcp::KcpCloseReason> closed;
    std::promise<kcp::KcpSnapshot> stopped;
    std::vector<Admission> admissions;
    std::vector<Terminal> terminals;
    std::vector<Receive> received;

    Side(Owner &owner, const Timebase &time, std::size_t logLimit)
        : owner(owner), time(time), logLimit(logLimit) {
        admissions.reserve(logLimit);
        terminals.reserve(logLimit);
        received.reserve(logLimit);
    }

    void start() {
        owner.call([&] {
            kcp::KcpOptions options;
            options.maxSessions = options.maxSessionsPerTurn = 1;
            options.maxSessionOpenings = 256;
            options.maxProtocolBytes = 1024 * 1024;
            options.maxPendingMessages = options.maxPendingMessagesPerSession;
            options.maxPendingBytes = options.maxPendingBytesPerSession;
            options.udp.maxPeers = options.udp.maxPeersPerTurn = 1;
            transport = kcp::KcpTransport::create(
                owner.loop, net::InetAddress("127.0.0.1", 0),
                {.message = [this](auto peer, std::string_view bytes) {
                    check(owner.executor.isInOwnerThread(), "client owner");
                    const auto now = time.now();
                    const auto id = messageId(bytes);
                    check(received.size() < logLimit, "receive log capacity");
                    received.push_back({id, peer->identity().conv, checkPayload(bytes, id), now,
                                        bytes.size()});
                },
                 .sendComplete = [this](auto identity, auto receipt) {
                    check(owner.executor.isInOwnerThread(), "terminal owner");
                    check(terminals.size() < logLimit, "terminal log capacity");
                    terminals.push_back({identity.conv, time.now(), receipt});
                },
                 .closed = [this](auto identity, auto reason) {
                    check(owner.executor.isInOwnerThread() && identity == session->identity(),
                          "close owner and generation");
                    closed.set_value(reason);
                },
                 .stopped = [this](auto snap) {
                    check(owner.executor.isInOwnerThread(), "stopped owner");
                    stopped.set_value(snap);
                }}, options);
            check(transport->start() == kcp::KcpResult::Accepted, "KCP start");
        });
    }
    net::InetAddress address() {
        return owner.call([&] { return transport->localAddress(); });
    }
    void open(net::InetAddress address, std::uint32_t conv) {
        owner.call([&, address] {
            closed = std::promise<kcp::KcpCloseReason>{};
            auto admitted = transport->openSession(address, conv);
            check(admitted.result == kcp::KcpResult::Accepted && admitted.session, "fresh KCP conv");
            session = std::move(admitted.session);
        });
    }
    kcp::KcpResult send(const std::shared_ptr<kcp::KcpSession> &peer, std::string_view bytes) {
        check(owner.executor.isInOwnerThread(), "send owner");
        const auto at = time.now();
        const auto admission = peer->send(bytes);
        check((admission.result == kcp::KcpResult::Accepted) == (admission.id != 0), "admission id");
        check(admissions.size() < logLimit, "admission log capacity");
        admissions.push_back({messageId(bytes), admission.id, peer->identity().conv, at,
                              bytes.size(), admission.result});
        return admission.result;
    }
    kcp::KcpSnapshot drain() {
        auto done = stopped.get_future();
        check(transport->stop() == kcp::KcpResult::Accepted, "KCP stop");
        auto snapshot = await(done);
        checkSettled(snapshot);
        owner.call([&] { transport.reset(); });
        return snapshot;
    }
    std::size_t pending() {
        return owner.call([&] { return transport->snapshot().pendingMessages; });
    }
};

class RecordingEndpoint final : public transport::TransportEndpoint {
    Side &side_;
    std::shared_ptr<kcp::KcpSession> session_;
    kcp_transport::KcpTransportEndpoint adapter_;

public:
    explicit RecordingEndpoint(Side &side)
        : side_(side), session_(side.session), adapter_(session_) {}
    transport::TransportSessionId id() const noexcept override { return adapter_.id(); }
    net::EventLoopExecutor ownerExecutor() const noexcept override { return adapter_.ownerExecutor(); }
    bool isOpen() const noexcept override { return adapter_.isOpen(); }
    transport::EndpointResult close(transport::CloseReason r) override { return adapter_.close(r); }
    DispatchResult requestClose(transport::CloseReason r) noexcept override {
        return adapter_.requestClose(r);
    }
    transport::EndpointResult send(std::string_view bytes) override {
        using E = transport::EndpointResult;
        switch (side_.send(session_, bytes)) {
        case kcp::KcpResult::Accepted: return E::Accepted;
        case kcp::KcpResult::Stopped: return E::Closed;
        case kcp::KcpResult::OwnerUnavailable: return E::OwnerUnavailable;
        case kcp::KcpResult::Overloaded:
        case kcp::KcpResult::NoMemory: return E::Overloaded;
        default: break;
        }
        check(false, "endpoint send result");
        std::abort();
    }
};

struct Relay {
    Owner &owner;
    const Timebase &time;
    std::shared_ptr<udp::UdpSocket> socket;
    std::promise<udp::UdpSnapshot> stopped;
    bool blocked{};
    std::uint64_t wireDrops{}, forwardRefusals{};

    net::InetAddress start(net::InetAddress left, net::InetAddress right) {
        return owner.call([&, left, right] {
            check(left.port() != right.port(), "relay port distinctness");
            socket = udp::UdpSocket::create(owner.loop, net::InetAddress("127.0.0.1", 0),
                {.datagram = [this, left, right](const auto &from, auto bytes, bool truncated) {
                    check(owner.executor.isInOwnerThread() && !truncated && from.isIpv4(),
                          "relay ownership and datagram");
                    const bool toClient = from.port() == left.port();
                    check(toClient || from.port() == right.port(), "unexpected relay source");
                    if (toClient && blocked) {
                        ++wireDrops;
                        return;
                    }
                    if (socket->sendTo(toClient ? right : left, bytes).result != udp::UdpResult::Accepted)
                        ++forwardRefusals;
                },
                 .stopped = [this](auto s) { stopped.set_value(s); }});
            check(socket->start() == udp::UdpResult::Accepted, "UDP relay start");
            return socket->localAddress();
        });
    }
    std::pair<Stamp, std::uint64_t> setBlocked(bool value) {
        return owner.call([&, value] {
            check(blocked != value, "fault transitions");
            blocked = value;
            return std::pair{time.now(), wireDrops};
        });
    }
    void drain() {
        auto done = stopped.get_future();
        check(socket->stop() == udp::UdpResult::Accepted, "UDP relay stop");
        checkSettled(await(done));
        owner.call([&] { socket.reset(); });
    }
};

struct Offer {
    Id seq;
    Kind kind;
    int mask;
    Stamp scheduled, published;
    std::size_t bytes;
    std::shared_ptr<broadcast::DispatchProgress> progress;
};
struct Fault {
    Id cycle{};
    Stamp startDue{}, started{}, stopDue{}, restored{}, rebind{};
    std::uint32_t oldConv{}, freshConv{};
    Id probe{};
    std::uint64_t wireDrops{};
};

enum class Action { Policy, Begin, Burst, Restore, Fanout };
struct Planned {
    Stamp offset;
    Action action;
    std::size_t cycle{};
};

std::ofstream logFile(const std::filesystem::path &dir, const char *name, std::string_view header) {
    std::ofstream out(dir / name, std::ios::binary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << header << '\n';
    return out;
}

struct Replay {
    Timebase time;
    Owner healthyOwner{"healthy-server"}, slowOwner{"slow-server"}, clientOwner{"clients"},
          management{"routing"};
    Side healthy{healthyOwner, time, healthyLogLimit}, slow{slowOwner, time, slowLogLimit},
         healthyClient{clientOwner, time, healthyLogLimit}, slowClient{clientOwner, time, slowLogLimit};
    Relay relay{clientOwner, time};
    net::InetAddress relayAddress;
    std::unique_ptr<game_session::SessionManager> sessions;
    std::unique_ptr<broadcast::BroadcastRouter> router;
    broadcast::BroadcastDispatcher dispatcher{broadcast::DispatchLimits{
        .maxEndpointsPerTask = 1, .maxBytesPerTask = profile::dispatchTaskBytes,
        .maxOutstandingTasksPerOwner = 64,
        .maxOutstandingBytesPerOwner = 256 * 1024, .maxGlobalOutstandingBytes = 512 * 1024,
        .lowPriorityOutstandingBytes = 128 * 1024}};
    std::shared_ptr<transport::TransportEndpoint> good, affected;
    game_session::SessionBinding goodBinding, slowBinding;
    std::vector<Offer> offers;
    std::vector<Fault> faults;
    Id nextSeq{};
    Stamp start{};

    game_session::SessionBinding bind(const char *player,
        const std::shared_ptr<transport::TransportEndpoint> &endpoint,
        game_session::AuthenticateStatus expected) {
        return management.call([&] {
            auto result = sessions->authenticate(player, endpoint);
            check(result.status == expected && result.session, "binding publication");
            return result.session->binding();
        });
    }
    void prepare() {
        for (auto *side : {&healthy, &slow, &healthyClient, &slowClient}) side->start();
        healthy.open(healthyClient.address(), healthyConv);
        healthyClient.open(healthy.address(), healthyConv);
        relayAddress = relay.start(slow.address(), slowClient.address());
        slow.open(relayAddress, firstSlowConv);
        slowClient.open(relayAddress, firstSlowConv);
        management.call([&] {
            sessions = std::make_unique<game_session::SessionManager>(management.loop);
            router = std::make_unique<broadcast::BroadcastRouter>(
                management.loop, broadcast::BroadcastLimits{
                    .softFanout = profile::routerFanoutLimit, .hardFanout = profile::routerFanoutLimit,
                    .softBytes = 64 * 1024, .hardBytes = 64 * 1024});
        });
        good = std::make_shared<RecordingEndpoint>(healthy);
        affected = std::make_shared<RecordingEndpoint>(slow);
        goodBinding = bind("healthy", good, game_session::AuthenticateStatus::Created);
        slowBinding = bind("slow", affected, game_session::AuthenticateStatus::Created);
        offers.reserve(slowLogLimit);
    }

    Id publish(Kind kind, int mask, Stamp due) {
        const auto id = ++nextSeq;
        auto bytes = std::make_shared<const std::string>(payload(kind, id));
        const auto published = time.now();
        auto result = management.call([&] {
            const std::array targets{broadcast::BroadcastTarget(good, goodBinding),
                                     broadcast::BroadcastTarget(affected, slowBinding)};
            auto sample = std::span(targets).subspan(
                mask == slowMask ? 1 : 0, mask == both ? profile::broadcastRecipients : 1);
            return dispatcher.dispatch(router->route(bytes, sample));
        });
        offers.push_back({id, kind, mask, due, published, bytes->size(), std::move(result.progress)});
        return id;
    }
    void pressure(Stamp due) {
        const auto first = nextSeq + 1;
        const auto published = time.now();
        for (int i = 0; i < burstSize; ++i) {
            const auto id = ++nextSeq;
            offers.push_back({id, Kind::Pressure, slowMask, due, published,
                              profile::pressureBodyBytes + sizeof("RESULT ") + std::to_string(id).size(),
                              {}});
        }
        slowOwner.call([&, first] {
            for (int i = 0; i < burstSize; ++i)
                slow.send(slow.session, payload(Kind::Pressure, first + i));
        });
    }
    void replace(Fault &fault, Stamp due) {
        auto left = slow.closed.get_future();
        auto right = slowClient.closed.get_future();
        fault.stopDue = due;
        fault.oldConv = slow.session->identity().conv;
        check(slow.session->requestClose(kcp::KcpCloseReason::Normal) == DispatchResult::Accepted &&
              slowClient.session->requestClose(kcp::KcpCloseReason::Normal) == DispatchResult::Accepted,
              "old generation close");
        const auto [stamp, count] = relay.setBlocked(false);
        fault.restored = stamp;
        fault.wireDrops = count - fault.wireDrops;
        check(fault.wireDrops > 0, "fault must drop real datagrams");
        check(await(left) == kcp::KcpCloseReason::Normal &&
              await(right) == kcp::KcpCloseReason::Normal, "old generation closed");
        fault.freshConv = fault.oldConv + 1;
        slow.open(relayAddress, fault.freshConv);
        slowClient.open(relayAddress, fault.freshConv);
        const auto old = slowBinding;
        affected = std::make_shared<RecordingEndpoint>(slow);
        slowBinding = bind("slow", affected, game_session::AuthenticateStatus::Rebound);
        check(!old.isCurrent() && slowBinding.isCurrent() &&
              old.sessionId() == slowBinding.sessionId() && old.transportId() != slowBinding.transportId(),
              "generation isolation");
        fault.rebind = time.now();
        fault.probe = publish(Kind::Probe, slowMask, due);
    }
    void run(std::size_t count) {
        faults.resize(count);
        const auto warmup = profile::warmupBroadcasts * tick.count();
        start = time.now() + warmup + 100000;
        for (int i = 0; i < profile::warmupBroadcasts; ++i) {
            const auto due = start - warmup + i * tick.count();
            time.at(due);
            publish(Kind::Warmup, both, due);
        }
        std::vector<Planned> plan;
        const auto duration = epoch.count() * static_cast<Stamp>(count);
        for (Stamp at = profile::firstBroadcastUs; at < duration; at += tick.count())
            plan.push_back({at, Action::Fanout});
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = epoch.count() * static_cast<Stamp>(i);
            plan.push_back({offset + profile::oversizeAtUs, Action::Policy, i});
            plan.push_back({offset + profile::faultBeginUs, Action::Begin, i});
            plan.push_back({offset + profile::pressureAtUs, Action::Burst, i});
            plan.push_back({offset + profile::faultRestoreUs, Action::Restore, i});
        }
        std::stable_sort(plan.begin(), plan.end(), [](auto a, auto b) { return a.offset < b.offset; });
        for (const auto &action : plan) {
            const auto due = start + action.offset;
            time.at(due);
            switch (action.action) {
            case Action::Policy: publish(Kind::Oversize, both, due); break;
            case Action::Begin: {
                const auto [now, drops] = relay.setBlocked(true);
                faults[action.cycle] = {.cycle = action.cycle + 1, .startDue = due,
                                        .started = now, .wireDrops = drops};
                break;
            }
            case Action::Burst: pressure(due); break;
            case Action::Restore: replace(faults[action.cycle], due); break;
            case Action::Fanout: publish(Kind::Fanout, both, due); break;
            }
        }
        time.at(start + duration);
    }
    std::array<kcp::KcpSnapshot, 4> stop() {
        const auto deadline = Clock::now() + 5s;
        while (dispatcher.outstanding().tasks || healthy.pending() || slow.pending()) {
            check(Clock::now() < deadline, "final ACK/dispatch drain deadline");
            std::this_thread::sleep_for(2ms);
        }
        check(!dispatcher.outstanding().bytes, "dispatch bytes drain");
        for (const auto &owner : dispatcher.outstandingStats().owners)
            check(!owner.tasks && !owner.bytes, "owner dispatch drain");
        dispatcher.shutdown();
        management.call([&] {
            sessions->shutdown();
            check(!sessions->size(), "session indexes drain");
            sessions.reset();
            router.reset();
        });
        const std::array snapshots{healthy.drain(), slow.drain(), healthyClient.drain(),
                                   slowClient.drain()};
        relay.drain();
        for (auto *owner : {&healthyOwner, &slowOwner, &clientOwner, &management})
            owner->call([&] {
                check(!owner->loop->pendingFunctorCount() && !owner->loop->pendingLifecycleNodeCount() &&
                      !owner->loop->attachedLifecycleNodeCount() && !owner->loop->callbackExceptionCount(),
                      "owner queues and lifecycle drain");
            });
        return snapshots;
    }
    void save(const std::filesystem::path &dir, const std::array<kcp::KcpSnapshot, 4> &snapshots) {
        std::filesystem::create_directories(dir);
        auto fingerprint = logFile(dir, "profile.sha256", profile::profileSha256);
        auto offerLog = logFile(dir, "offers.csv",
            "seq,kind,mask,scheduled_us,published_us,bytes,dispatch_accepted,dispatch_dropped,byte_limit");
        for (const auto &o : offers) {
            broadcast::DispatchProgressSnapshot result{};
            if (o.progress) {
                result = o.progress->snapshot();
                check(result.complete && !result.outstandingTasks && !result.outstandingBytes,
                      "dispatch result remains live");
            }
            offerLog << o.seq << ',' << name(o.kind) << ',' << o.mask << ',' << o.scheduled << ','
                     << o.published << ',' << o.bytes << ',' << result.acceptedEndpoints << ','
                     << result.droppedEndpoints << ','
                     << result.reasonCount(broadcast::BroadcastReason::DispatchTaskByteLimit) << '\n';
        }
        auto tx = logFile(dir, "admission.csv", "side,seq,conv,send_id,at_us,bytes,result");
        auto terminal = logFile(dir, "terminals.csv", "side,conv,send_id,at_us,bytes,status");
        auto receive = logFile(dir, "receive.csv", "side,seq,conv,kind,at_us,bytes");
        for (const auto &[side, pair] : std::array{
                 std::pair{"healthy", std::pair{&healthy, &healthyClient}},
                 std::pair{"slow", std::pair{&slow, &slowClient}}}) {
            check(pair.first->received.empty() && pair.second->admissions.empty(), "wire direction");
            for (const auto &a : pair.first->admissions)
                tx << side << ',' << a.seq << ',' << a.conv << ',' << a.sendId << ',' << a.at << ','
                   << a.bytes << ',' << admissionName(a.result) << '\n';
            for (const auto &t : pair.first->terminals)
                terminal << side << ',' << t.conv << ',' << t.completion.id << ',' << t.at << ','
                         << t.completion.bytes << ',' << terminalName(t.completion.status) << '\n';
            for (const auto &r : pair.second->received)
                receive << side << ',' << r.seq << ',' << r.conv << ',' << name(r.kind) << ','
                        << r.at << ',' << r.bytes << '\n';
        }
        auto faultLog = logFile(dir, "faults.csv",
            "cycle,start_due_us,start_us,stop_due_us,restored_us,rebind_us,old_conv,new_conv,probe_id,wire_drops");
        for (const auto &f : faults)
            faultLog << f.cycle << ',' << f.startDue << ',' << f.started << ',' << f.stopDue << ','
                     << f.restored << ',' << f.rebind << ',' << f.oldConv << ',' << f.freshConv << ','
                     << f.probe << ',' << f.wireDrops << '\n';
        auto final = logFile(dir, "final.csv",
            "start_us,epoch_us,epochs,relay_wire_drops,relay_refusals,healthy_accepted,slow_accepted,"
            "healthy_opened,slow_opened,healthy_client_opened,slow_client_opened,residual");
        final << start << ',' << epoch.count() << ',' << faults.size() << ',' << relay.wireDrops << ','
              << relay.forwardRefusals << ',' << snapshots[0].accepted << ',' << snapshots[1].accepted << ','
              << snapshots[0].opened << ',' << snapshots[1].opened << ',' << snapshots[2].opened << ','
              << snapshots[3].opened << ",0\n";
    }
};
} // namespace

int main(int argc, char **argv) {
    if (argc != 3 || (std::string_view(argv[1]) != std::to_string(profile::measureEpochs) &&
                      std::string_view(argv[1]) != std::to_string(profile::smokeEpochs))) {
        std::cerr << "usage: gamenet_kcp_gateway_replay <" << profile::measureEpochs << '|'
                  << profile::smokeEpochs << "> <new-output-directory>\n";
        return 2;
    }
    const std::filesystem::path out = argv[2];
    if (std::filesystem::exists(out)) return 2;
    Replay replay;
    replay.prepare();
    replay.run(static_cast<std::size_t>(std::stoi(argv[1])));
    const auto settled = replay.stop();
    replay.save(out, settled);
    std::cout << "Gateway KCP replay completed with " << argv[1] << " epochs\n";
}
