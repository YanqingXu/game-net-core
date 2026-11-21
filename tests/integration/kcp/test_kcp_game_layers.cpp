// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// KCP-C3: real endpoints, management bindings, logic and owner-side broadcast.
#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/game_logic/LogicLoop.h"
#include "gamenet/game_session/SessionManager.h"
#include "gamenet/kcp_transport/KcpTransportEndpoint.h"
#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"
#include "support/RawUdp.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace gamenet;
using namespace std::chrono_literals;
namespace {
template <class T> T ready(std::future<T> &f) {
    GAMENET_TEST_ASSERT(f.wait_for(15s) == std::future_status::ready);
    return f.get();
}
template <class F> void eventually(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate()) {
        GAMENET_TEST_ASSERT(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

// Only the external coordinator waits for owners. Owners never call call().
class Owner {
    std::promise<net::EventLoop *> published_;
    std::jthread thread_;

  public:
    net::EventLoop *loop{};
    net::EventLoopExecutor executor;
    Owner()
        : thread_([this] {
              net::EventLoop loop({.maxPendingFunctors = 64,
                                   .reservedPendingFunctors = 8,
                                   .maxFunctorsPerIteration = 8});
              published_.set_value(&loop);
              loop.loop();
          }) {
        auto f = published_.get_future();
        loop = ready(f);
        executor = loop->executor();
    }
    ~Owner() {
        loop->quit();
        thread_.join();
    }
    template <class F> auto call(F f) {
        GAMENET_TEST_ASSERT(!executor.isInOwnerThread());
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        auto future = task->get_future();
        GAMENET_TEST_ASSERT(executor.post([task] { (*task)(); }) == net::PostResult::Accepted);
        return ready(future);
    }
};
class Barrier {
  protected:
    struct State {
        std::promise<void> entered, release;
        std::shared_future<void> released{release.get_future().share()};
        void enter() {
            entered.set_value();
            GAMENET_TEST_ASSERT(released.wait_for(15s) == std::future_status::ready);
        }
    };
    std::shared_ptr<State> state_{std::make_shared<State>()};
    bool open_{};

  public:
    void enter() {
        auto state = state_;
        state->enter();
    }
    void wait() {
        auto f = state_->entered.get_future();
        ready(f);
    }
    void open() {
        if (!open_) {
            open_ = true;
            state_->release.set_value();
        }
    }
    ~Barrier() { open(); }
};
class Park : public Barrier {
  public:
    explicit Park(Owner &owner) {
        GAMENET_TEST_ASSERT(owner.executor.post([state = state_] { state->enter(); }) ==
                            net::PostResult::Accepted);
        wait();
    }
};

void settledSocket(const udp::UdpSnapshot &s) {
    GAMENET_TEST_ASSERT(s.phase == udp::UdpPhase::Stopped && !s.callbackExceptions);
    test::verifyUdpLedger(s);
}

void settled(const kcp::KcpSnapshot &s) {
    GAMENET_TEST_ASSERT(s.phase == udp::UdpPhase::Stopped && s.opened == s.closed);
    GAMENET_TEST_ASSERT(s.accepted == s.acknowledged + s.cancelled + s.failed);
    GAMENET_TEST_ASSERT(!s.sessions && !s.pendingMessages && !s.pendingBytes && !s.protocolBytes &&
                        !s.scheduledPosts && !s.callbackExceptions);
    GAMENET_TEST_ASSERT(s.udp.phase == udp::UdpPhase::Stopped && s.udp.opened == s.udp.closed &&
                        !s.udp.peers && !s.udp.pendingDatagrams && !s.udp.pendingBytes &&
                        !s.udp.scheduledPosts && !s.udp.callbackExceptions);
    GAMENET_TEST_ASSERT(s.udp.acceptedDatagrams ==
                        s.udp.sentDatagrams + s.udp.cancelledDatagrams + s.udp.failedDatagrams);
    GAMENET_TEST_ASSERT(s.udp.acceptedBytes ==
                        s.udp.sentBytes + s.udp.cancelledBytes + s.udp.failedBytes);
    settledSocket(s.udp.socket);
}

struct Receipt {
    std::size_t bytes;
    kcp::KcpSendStatus status;
    std::uint64_t id{};
};

struct Side {
    Owner &owner;
    std::shared_ptr<kcp::KcpTransport> service;
    std::shared_ptr<kcp::KcpSession> session;
    std::shared_ptr<transport::TransportEndpoint> endpoint;
    std::vector<std::string> received;
    std::map<std::uint64_t, kcp::KcpSendCompletion> receipts;
    std::optional<kcp::KcpCloseReason> closeReason;
    std::function<void(std::string)> input;
    std::promise<kcp::KcpSnapshot> stopped;
    explicit Side(Owner &o) : owner(o) {}
    void create(bool v6) {
        owner.call([&] {
            kcp::KcpOptions options;
            options.maxPendingMessagesPerSession = 2;
            options.maxPendingMessages = 2;
            options.sendTimeout = 15s;
            service = kcp::KcpTransport::create(
                owner.loop, net::InetAddress(v6 ? "::1" : "127.0.0.1", 0),
                {.message =
                     [this](auto, std::string_view bytes) {
                         GAMENET_TEST_ASSERT(owner.executor.isInOwnerThread());
                         GAMENET_TEST_ASSERT(received.size() < 16);
                         received.emplace_back(bytes);
                         if (input)
                             input(std::string(bytes));
                     },
                 .sendComplete =
                     [this](auto identity, auto receipt) {
                         GAMENET_TEST_ASSERT(owner.executor.isInOwnerThread());
                         GAMENET_TEST_ASSERT(identity == session->identity());
                         GAMENET_TEST_ASSERT(receipt.id && receipts.size() < 16);
                         GAMENET_TEST_ASSERT(receipts.emplace(receipt.id, receipt).second);
                     },
                 .closed =
                     [this](auto identity, auto reason) {
                         GAMENET_TEST_ASSERT(owner.executor.isInOwnerThread());
                         GAMENET_TEST_ASSERT(identity == session->identity() && !closeReason);
                         closeReason = reason;
                     },
                 .stopped =
                     [this](auto s) {
                         settled(s);
                         GAMENET_TEST_ASSERT(s.accepted == receipts.size());
                         stopped.set_value(s);
                     }},
                options);
            GAMENET_TEST_ASSERT(service->start() == kcp::KcpResult::Accepted);
        });
    }
    kcp::KcpSnapshot snapshot() {
        return owner.call([&] { return service->snapshot(); });
    }
    std::uint64_t send(std::string_view bytes) {
        return owner.call([&, bytes] {
            const auto admission = session->send(bytes);
            GAMENET_TEST_ASSERT(admission.result == kcp::KcpResult::Accepted && admission.id);
            return admission.id;
        });
    }
    void expectReceipts(std::initializer_list<Receipt> expected) {
        owner.call([&] {
            GAMENET_TEST_ASSERT(receipts.size() == expected.size());
            // Each broadcast payload on this side has a distinct length.
            // Direct KcpSession sends also expose their exact admission id.
            for (const auto &entry : expected) {
                const auto matches = std::ranges::count_if(receipts, [&](const auto &pair) {
                    const auto &terminal = pair.second;
                    return terminal.bytes == entry.bytes && terminal.status == entry.status &&
                           (!entry.id || terminal.id == entry.id);
                });
                GAMENET_TEST_ASSERT(matches == 1);
            }
        });
    }
};

class AckDropRelay {
    Owner &owner_;
    std::shared_ptr<udp::UdpSocket> socket_;
    std::atomic<std::size_t> drops_{};
    std::promise<udp::UdpSnapshot> stopped_;

  public:
    explicit AckDropRelay(Owner &owner) : owner_(owner) {}

    net::InetAddress start(bool v6, net::InetAddress sender, net::InetAddress receiver) {
        GAMENET_TEST_ASSERT(sender.port() != receiver.port());
        return owner_.call([this, v6, sender, receiver] {
            const net::InetAddress local(v6 ? "::1" : "127.0.0.1", 0);
            udp::UdpOptions options;
            options.maxPendingDatagrams = 4;
            options.maxPendingBytes = 4 * options.maxDatagramBytes;
            socket_ = udp::UdpSocket::create(
                owner_.loop, local,
                {.datagram = [this, sender, receiver](const auto &from, auto bytes, bool truncated) {
                     GAMENET_TEST_ASSERT(owner_.executor.isInOwnerThread() && !truncated);
                     GAMENET_TEST_ASSERT(from.family() == sender.family());
                     if (from.port() == sender.port()) {
                         GAMENET_TEST_ASSERT(socket_->sendTo(receiver, bytes).result ==
                                             udp::UdpResult::Accepted);
                         return;
                     }
                     GAMENET_TEST_ASSERT(from.port() == receiver.port());
                     // The impaired link carries application data in one direction.
                     // Its return datagrams must contain only standard KCP ACK segments.
                     constexpr std::size_t headerBytes = 24;
                     constexpr unsigned char ack = 82;
                     GAMENET_TEST_ASSERT(!bytes.empty() && bytes.size() % headerBytes == 0);
                     for (std::size_t i = 0; i < bytes.size(); i += headerBytes) {
                         GAMENET_TEST_ASSERT(static_cast<unsigned char>(bytes[i + 4]) == ack);
                         GAMENET_TEST_ASSERT(bytes.substr(i + 20, 4) ==
                                             std::string_view("\0\0\0\0", 4));
                     }
                     drops_.fetch_add(1, std::memory_order_relaxed);
                 },
                 .stopped = [this](auto s) { stopped_.set_value(s); }},
                options);
            GAMENET_TEST_ASSERT(socket_->start() == udp::UdpResult::Accepted);
            return socket_->localAddress();
        });
    }
    std::size_t drops() const { return drops_.load(std::memory_order_relaxed); }
    void stop() {
        auto future = stopped_.get_future();
        GAMENET_TEST_ASSERT(socket_->stop() == udp::UdpResult::Accepted);
        const auto snapshot = ready(future);
        settledSocket(snapshot);
        GAMENET_TEST_ASSERT(!snapshot.failedDatagrams && snapshot.sentDatagrams >= 3);
        owner_.call([&] { socket_.reset(); });
    }
};

struct Link {
    Side server, client;
    Link(Owner &network, Owner &remote, bool v6, std::uint32_t conv,
         AckDropRelay *relay = nullptr)
        : server(network), client(remote) {
        server.create(v6);
        client.create(v6);
        auto a = network.call([&] { return server.service->localAddress(); });
        auto b = remote.call([&] { return client.service->localAddress(); });
        if (relay)
            a = b = relay->start(v6, a, b);
        network.call([&] {
            auto opened = server.service->openSession(b, conv);
            GAMENET_TEST_ASSERT(opened.result == kcp::KcpResult::Accepted && opened.session);
            server.session = std::move(opened.session);
            server.endpoint = std::make_shared<kcp_transport::KcpTransportEndpoint>(server.session);
        });
        remote.call([&] {
            auto opened = client.service->openSession(a, conv);
            GAMENET_TEST_ASSERT(opened.result == kcp::KcpResult::Accepted && opened.session);
            client.session = std::move(opened.session);
        });
    }
    std::array<kcp::KcpSnapshot, 2> stop() {
        auto a = server.stopped.get_future(), b = client.stopped.get_future();
        (void)server.service->stop();
        (void)client.service->stop();
        std::array snapshots{ready(a), ready(b)};
        for (auto &snapshot : snapshots)
            settled(snapshot);
        server.owner.call([&] {
            server.input = {};
            server.service.reset();
        });
        client.owner.call([&] {
            client.input = {};
            client.service.reset();
        });
        return snapshots;
    }
    std::uint64_t send(std::string_view bytes) {
        return client.send(bytes);
    }
};
game_logic::GameCommand command(game_session::SessionBinding b, std::string payload) {
    return {.sessionId = b.sessionId(),
            .transportId = b.transportId(),
            .binding = b,
            .payload = std::move(payload)};
}
void complete(const broadcast::DispatchSummary &s, std::size_t accepted,
              broadcast::BroadcastReason reason = broadcast::BroadcastReason::None) {
    eventually([&] { return s.progress->snapshot().complete; });
    const auto p = s.progress->snapshot();
    GAMENET_TEST_ASSERT(p.acceptedEndpoints == accepted && !p.outstandingTasks &&
                        !p.outstandingBytes);
    GAMENET_TEST_ASSERT(p.droppedEndpoints == (reason == broadcast::BroadcastReason::None ? 0 : 1));
    if (reason != broadcast::BroadcastReason::None)
        GAMENET_TEST_ASSERT(p.reasonCount(reason) == 1);
}
void empty(const broadcast::BroadcastDispatcher &d) {
    eventually([&] { return d.outstanding().tasks == 0; });
    GAMENET_TEST_ASSERT(d.outstanding().bytes == 0);
    for (const auto &o : d.outstandingStats().owners)
        GAMENET_TEST_ASSERT(!o.tasks && !o.bytes);
}

using Status = game_session::AuthenticateStatus;
game_session::SessionBinding authenticateOn(
    Owner &management, game_session::SessionManager &manager, const char *player,
    const std::shared_ptr<transport::TransportEndpoint> &endpoint, Status expected) {
    return management.call([&] {
        auto r = manager.authenticate(player, endpoint);
        GAMENET_TEST_ASSERT(r.status == expected && r.session);
        GAMENET_TEST_ASSERT(manager.findByTransport(endpoint->id()) == r.session);
        return r.session->binding();
    });
}

void run(bool v6) {
    Owner management, logicOwner, networkA, networkB, remoteA, remoteB;
    Link old(networkA, remoteA, v6, 101), replacement(networkB, remoteB, v6, 102),
        healthy(networkB, remoteB, v6, 103);
    GAMENET_TEST_ASSERT(networkA.executor.id() != networkB.executor.id() &&
                        management.executor.id() != logicOwner.executor.id());
    std::unique_ptr<game_session::SessionManager> manager;
    std::unique_ptr<broadcast::BroadcastRouter> router;
    std::unique_ptr<game_logic::LogicLoop> logic;
    broadcast::BroadcastDispatcher dispatcher;
    management.call([&] {
        manager = std::make_unique<game_session::SessionManager>(management.loop);
        router = std::make_unique<broadcast::BroadcastRouter>(management.loop);
    });
    const auto authenticate = [&](const char *player, auto endpoint, auto expected) {
        return authenticateOn(management, *manager, player, endpoint, expected);
    };
    auto binding = authenticate("player", old.server.endpoint, Status::Created);
    auto healthyBinding = authenticate("healthy", healthy.server.endpoint, Status::Created);
    const auto route = [&](auto endpoint, auto b, std::string bytes) {
        return management.call([&, endpoint, b, bytes = std::move(bytes)] {
            const std::array targets{broadcast::BroadcastTarget(endpoint, b)};
            return router->route(std::make_shared<const std::string>(bytes), targets);
        });
    };
    const auto dispatch = [&](auto endpoint, auto b, std::string bytes) {
        return dispatcher.dispatch(route(endpoint, b, std::move(bytes)));
    };

    // Same-endpoint refresh keeps the transport open: closed-state checks alone
    // cannot enforce revocation of an already-routed or already-queued binding.
    auto stalePlan = route(old.server.endpoint, binding, "stale-plan");
    auto refreshed = authenticate("player", old.server.endpoint, Status::Existing);
    GAMENET_TEST_ASSERT(!binding.isCurrent() && old.server.endpoint->isOpen());
    complete(dispatcher.dispatch(std::move(stalePlan)), 0,
             broadcast::BroadcastReason::OfflineSession);
    binding = refreshed;
    {
        Park parked(networkA);
        auto queued = dispatch(old.server.endpoint, binding, "stale-task");
        GAMENET_TEST_ASSERT(queued.scheduledTasks == 1);
        refreshed = authenticate("player", old.server.endpoint, Status::Existing);
        GAMENET_TEST_ASSERT(!binding.isCurrent() && old.server.endpoint->isOpen());
        parked.open();
        complete(queued, 0, broadcast::BroadcastReason::OfflineSession);
        binding = refreshed;
    }
    GAMENET_TEST_ASSERT(old.server.snapshot().accepted == 0);

    // Hold real remote input/ACK service. Three owner tasks encounter the actual
    // two-message KCP cap. The other owner still delivers and ACKs healthy work.
    {
        Park remote(remoteA), network(networkA);
        std::array results{dispatch(old.server.endpoint, binding, "pressure-0"),
                           dispatch(old.server.endpoint, binding, "pressure-1"),
                           dispatch(old.server.endpoint, binding, "pressure-2")};
        auto good = dispatch(healthy.server.endpoint, healthyBinding, "healthy-progress");
        complete(good, 1);
        eventually([&] { return healthy.server.snapshot().acknowledged == 1; });
        network.open();
        complete(results[0], 1);
        complete(results[1], 1);
        complete(results[2], 0, broadcast::BroadcastReason::EndpointOverloaded);
        auto s = old.server.snapshot();
        GAMENET_TEST_ASSERT(s.accepted == 2 && s.pendingMessages == 2 && !s.acknowledged);
        remote.open();
    }
    eventually([&] { return old.server.snapshot().acknowledged == 2; });
    remoteA.call([&] {
        GAMENET_TEST_ASSERT(
            (old.client.received == std::vector<std::string>{"pressure-0", "pressure-1"}));
    });
    empty(dispatcher);

    // Outstanding admission rolls back independently of transport admission.
    {
        Park parked(networkA);
        broadcast::BroadcastDispatcher tasks({.maxOutstandingTasksPerOwner = 1});
        auto first = tasks.dispatch(route(old.server.endpoint, binding, "budget-a"));
        auto refused = tasks.dispatch(route(old.server.endpoint, binding, "budget-b"));
        complete(refused, 0, broadcast::BroadcastReason::OwnerOutstandingTaskLimit);
        GAMENET_TEST_ASSERT(tasks.outstanding().tasks == 1);
        parked.open();
        complete(first, 1);
        empty(tasks);
    }
    eventually([&] { return !old.server.snapshot().pendingMessages; });

    for (bool global : {false, true}) {
        Park parked(networkA);
        broadcast::DispatchLimits limits;
        if (global) {
            limits.maxGlobalOutstandingBytes = 8;
            limits.lowPriorityOutstandingBytes = 8;
        } else
            limits.maxOutstandingBytesPerOwner = 8;
        broadcast::BroadcastDispatcher bytes(limits);
        auto first = bytes.dispatch(route(old.server.endpoint, binding, "12345678"));
        auto refused =
            bytes.dispatch(global ? route(healthy.server.endpoint, healthyBinding, "abcdefgh")
                                  : route(old.server.endpoint, binding, "abcdefgh"));
        complete(refused, 0,
                 global ? broadcast::BroadcastReason::GlobalOutstandingByteLimit
                        : broadcast::BroadcastReason::OwnerOutstandingByteLimit);
        GAMENET_TEST_ASSERT(bytes.outstanding().bytes == 8);
        parked.open();
        complete(first, 1);
        empty(bytes);
        eventually([&] { return !old.server.snapshot().pendingMessages; });
    }

    std::atomic<unsigned> handled{}, outputs{};
    Barrier handlerBarrier;
    logicOwner.call([&] {
        logic = std::make_unique<game_logic::LogicLoop>(
            logicOwner.loop,
            game_logic::LogicLoopOptions{.tickInterval = 1ms,
                                         .maxCommandsPerTick = 1,
                                         .queueLimits = {.maxCommands = 2,
                                                         .maxQueuedBytes = 32768,
                                                         .maxPayloadBytes = 16384}});
        logic->setHandler([&](auto c) -> std::optional<game_logic::GameCommand> {
            GAMENET_TEST_ASSERT(logicOwner.executor.isInOwnerThread());
            ++handled;
            if (c.payload == "revoke-in-handler")
                handlerBarrier.enter();
            return c;
        });
        logic->setOutputCallback([&](auto c) {
            ++outputs;
            GAMENET_TEST_ASSERT(management.executor.post([&, c = std::move(c)] {
                auto current = manager->findByTransport(c.transportId);
                GAMENET_TEST_ASSERT(current && c.binding.isCurrent());
                const std::array targets{
                    broadcast::BroadcastTarget(current->endpoint(), c.binding)};
                (void)dispatcher.dispatch(
                    router->route(std::make_shared<const std::string>(c.payload), targets));
            }) == net::PostResult::Accepted);
        });
        logic->start();
    });
    const auto attach = [&](Link &link, auto b) {
        link.server.owner.call([&, b] {
            link.server.input = [&, b](std::string bytes) {
                GAMENET_TEST_ASSERT(logic->submit(command(b, std::move(bytes))) ==
                                    game_logic::SubmitResult::Accepted);
            };
        });
    };
    attach(old, binding);
    {
        Park parked(logicOwner);
        old.send("queued-before-rebind");
        eventually([&] { return logic->queueSnapshot().depth == 1; });
        refreshed = authenticate("player", replacement.server.endpoint, Status::Rebound);
        GAMENET_TEST_ASSERT(!binding.isCurrent());
        // A real old owner submits late offline through the asynchronous API.
        std::promise<void> offlineDone;
        auto offline = offlineDone.get_future();
        networkA.call([&] {
            GAMENET_TEST_ASSERT(manager->postOffline(old.server.endpoint->id(), [&](auto result) {
                GAMENET_TEST_ASSERT(management.executor.isInOwnerThread());
                GAMENET_TEST_ASSERT(result == DispatchResult::PolicyRejected);
                GAMENET_TEST_ASSERT(manager->findByPlayer("player")->binding().generation() ==
                                    refreshed.generation());
                offlineDone.set_value();
            }) == DispatchResult::Accepted);
        });
        ready(offline);
        parked.open();
    }
    eventually([&] { return logic->queueSnapshot().droppedStale == 1; });
    GAMENET_TEST_ASSERT(!handled && !outputs);
    binding = refreshed;
    attach(replacement, binding);
    replacement.send("revoke-in-handler");
    handlerBarrier.wait();
    refreshed = authenticate("player", replacement.server.endpoint, Status::Existing);
    handlerBarrier.open();
    eventually([&] { return logic->queueSnapshot().droppedStale == 2; });
    GAMENET_TEST_ASSERT(handled == 1 && outputs == 0);
    binding = refreshed;
    attach(replacement, binding);
    const std::string payload(12000, 'x');
    replacement.send(payload);
    eventually([&] { return replacement.server.snapshot().acknowledged == 1; });
    remoteB.call([&] {
        GAMENET_TEST_ASSERT(replacement.client.received == std::vector<std::string>{payload});
    });
    GAMENET_TEST_ASSERT(handled == 2 && outputs == 1);

    // Count/byte admission and stop discard are exact while the logic owner is
    // parked. No queued command can run after owner-side stop.
    {
        Park parked(logicOwner);
        GAMENET_TEST_ASSERT(logic->submit(command(binding, "discard-0")) ==
                            game_logic::SubmitResult::Accepted);
        GAMENET_TEST_ASSERT(logic->submit(command(binding, "discard-1")) ==
                            game_logic::SubmitResult::Accepted);
        GAMENET_TEST_ASSERT(logic->submit(command(binding, "refused")) ==
                            game_logic::SubmitResult::QueueFull);
        GAMENET_TEST_ASSERT(logicOwner.executor.post([&] {
            auto stopped = logic->stop();
            // The next timer may precede this post, so binding revocation below
            // also forbids any business/output side effect from those commands.
            GAMENET_TEST_ASSERT(stopped.droppedCommands <= 2);
        }) == net::PostResult::Accepted);
        Park remotePark(remoteB);
        networkB.call([&] {
            GAMENET_TEST_ASSERT(replacement.server.endpoint->send("cancel-at-shutdown") ==
                                transport::EndpointResult::Accepted);
            GAMENET_TEST_ASSERT(replacement.server.service->snapshot().pendingMessages == 1);
        });
        Park networkPark(networkB);
        auto queuedClose = dispatch(replacement.server.endpoint, binding, "queued-before-shutdown");
        unsigned queued = 0;
        while (networkB.executor.post([] {}) == net::PostResult::Accepted)
            GAMENET_TEST_ASSERT(++queued <= 64);
        GAMENET_TEST_ASSERT(queued > 0);
        auto refused = dispatch(healthy.server.endpoint, healthyBinding, "queue-full");
        complete(refused, 0, broadcast::BroadcastReason::DispatchQueueFull);
        management.call([&] {
            manager->shutdown();
            GAMENET_TEST_ASSERT(manager->size() == 0);
            GAMENET_TEST_ASSERT(!replacement.server.endpoint->isOpen() &&
                                !healthy.server.endpoint->isOpen());
            GAMENET_TEST_ASSERT(manager->postHeartbeat(binding.transportId()) ==
                                DispatchResult::Shutdown);
        });
        networkPark.open();
        complete(queuedClose, 0, broadcast::BroadcastReason::OfflineSession);
        eventually([&] { return replacement.server.snapshot().cancelled == 1; });
        remotePark.open();
        parked.open();
    }
    // Quiesce input bridges before releasing the separately owned logic facade.
    networkA.call([&] { old.server.input = {}; });
    networkB.call([&] {
        replacement.server.input = {};
        healthy.server.input = {};
    });
    logicOwner.call([&] {
        auto s = logic->queueSnapshot();
        GAMENET_TEST_ASSERT(!s.depth && !s.queuedBytes && !s.accepting);
        GAMENET_TEST_ASSERT(s.droppedStale + s.droppedOnStop == 4);
        GAMENET_TEST_ASSERT(logic->submit(command(binding, "stale-after-stop")) ==
                            game_logic::SubmitResult::StaleBinding);
        GAMENET_TEST_ASSERT(logic->submit(command({}, "after-stop")) ==
                            game_logic::SubmitResult::Stopped);
        logic.reset();
    });
    GAMENET_TEST_ASSERT(handled == 2 && outputs == 1);

    dispatcher.shutdown();
    empty(dispatcher);
    management.call([&] {
        GAMENET_TEST_ASSERT(manager->size() == 0);
        manager.reset();
        router.reset();
    });
    old.stop();
    replacement.stop();
    healthy.stop();
    for (auto *o : {&networkA, &networkB, &remoteA, &remoteB})
        o->call([&] { GAMENET_TEST_ASSERT(!o->loop->attachedLifecycleNodeCount()); });
    std::cout << "KCP_C3 family=" << (v6 ? 6 : 4) << " stale_plan=1 stale_task=1 stale_logic=2"
              << " pending_refusal=1 task_refusal=1 byte_refusal=2 queue_refusal=1"
              << " pending_cancel=1 healthy_progress=1 zero_residue=1\n";
}

struct ChurnOwners {
    Owner management, logic, impaired, replacement, healthy, remote;

    void checkDrained() {
        for (auto *owner : {&management, &logic, &impaired, &replacement, &healthy, &remote}) {
            owner->call([&] {
                auto &loop = *owner->loop;
                GAMENET_TEST_ASSERT(!loop.pendingFunctorCount() && !loop.pendingControlSourceCount() &&
                                    !loop.pendingLifecycleNodeCount() && !loop.attachedLifecycleNodeCount() &&
                                    !loop.callbackExceptionCount());
                // The owners survive the round. A retired timer must leave no entry
                // even if its callback would now be a no-op on a revoked lifetime.
                loop.setEventLoopMetricCallback([](const auto &metric) {
                    GAMENET_TEST_ASSERT(metric.event != net::EventLoopMetricEvent::TimersDrained);
                });
                net::detail::EventLoopActiveBatchHarness::runExpiredTimers(loop, base::now() + 1h);
                loop.setEventLoopMetricCallback({});
            });
        }
    }
};

struct ChurnStats {
    std::size_t ackDrops{}, staleBroadcast{}, staleLogic{}, healthyReceived{};
    std::uint64_t accepted{}, acknowledged{}, cancelled{}, failed{};
};

ChurnStats churnRound(ChurnOwners &owners, bool v6, std::uint32_t &conv) {
    using Reason = broadcast::BroadcastReason;
    using Send = kcp::KcpSendStatus;
    using Close = kcp::KcpCloseReason;
    using Binding = game_session::SessionBinding;
    auto &management = owners.management;
    auto &logicOwner = owners.logic;
    AckDropRelay relay(owners.remote);
    Link old(owners.impaired, owners.remote, v6, ++conv, &relay),
        replacement(owners.replacement, owners.remote, v6, ++conv),
        healthy(owners.healthy, owners.remote, v6, ++conv);
    GAMENET_TEST_ASSERT(owners.impaired.executor.id() != owners.replacement.executor.id());
    GAMENET_TEST_ASSERT(owners.healthy.executor.id() != owners.replacement.executor.id());
    GAMENET_TEST_ASSERT(owners.healthy.executor.id() != owners.impaired.executor.id());

    auto [manager, router] = management.call([&] {
        return std::pair{std::make_unique<game_session::SessionManager>(management.loop),
                         std::make_unique<broadcast::BroadcastRouter>(management.loop)};
    });
    broadcast::BroadcastDispatcher dispatcher;
    auto binding = authenticateOn(management, *manager, "churn", old.server.endpoint,
                                  Status::Created);
    const auto healthyBinding =
        authenticateOn(management, *manager, "healthy", healthy.server.endpoint, Status::Created);
    const std::string unacknowledged(96, 'u');
    const auto unacknowledgedId = old.server.send(unacknowledged);
    eventually([&] { return relay.drops() >= 1; });
    const auto expectPending = [&] {
        const auto s = old.server.snapshot();
        GAMENET_TEST_ASSERT(s.accepted == 1 && s.pendingMessages == 1 &&
                            s.pendingBytes == unacknowledged.size() &&
                            !s.acknowledged && !s.cancelled && !s.failed);
    };
    expectPending();

    const std::string livePayload(12000, 'r');
    std::size_t handled{}, outputs{}; // Read or updated only on the logic owner.
    std::promise<broadcast::DispatchSummary> liveOutput;
    auto output = liveOutput.get_future();
    auto logic = logicOwner.call([&] {
        auto loop = std::make_unique<game_logic::LogicLoop>(
            logicOwner.loop,
            game_logic::LogicLoopOptions{.tickInterval = 1ms, .maxCommandsPerTick = 1,
                                         .queueLimits = {.maxCommands = 2,
                                                         .maxQueuedBytes = 16384,
                                                         .maxPayloadBytes = 16384}});
        loop->setHandler([&](auto c) -> std::optional<game_logic::GameCommand> {
            GAMENET_TEST_ASSERT(logicOwner.executor.isInOwnerThread());
            GAMENET_TEST_ASSERT(c.binding.isCurrent() && c.payload == livePayload);
            ++handled;
            return c;
        });
        loop->setOutputCallback([&](auto c) {
            GAMENET_TEST_ASSERT(logicOwner.executor.isInOwnerThread());
            ++outputs;
            GAMENET_TEST_ASSERT(management.executor.post([&, c = std::move(c)] {
                auto current = manager->findByTransport(c.transportId);
                GAMENET_TEST_ASSERT(current && c.binding.isCurrent());
                GAMENET_TEST_ASSERT(current->endpoint() == replacement.server.endpoint);
                const std::array targets{broadcast::BroadcastTarget(current->endpoint(), c.binding)};
                liveOutput.set_value(dispatcher.dispatch(
                    router->route(std::make_shared<const std::string>(c.payload), targets)));
            }) == net::PostResult::Accepted);
        });
        loop->start();
        return loop;
    });

    ChurnStats stats;
    std::vector<std::string> healthyPayloads;
    const auto revokeQueued = [&](Side &side, Binding before, auto transition) {
        Park blockedLogic(logicOwner);
        // Submit owned data from the affected network owner without sending any
        // reverse application packets carrying the impaired peer's cumulative ACK.
        side.owner.call([&] {
            GAMENET_TEST_ASSERT(logic->submit(command(before, "stale-command")) ==
                                game_logic::SubmitResult::Accepted);
        });
        GAMENET_TEST_ASSERT(logic->queueSnapshot().depth == 1);
        Park blockedNetwork(side.owner);
        healthyPayloads.emplace_back(48 + healthyPayloads.size(), 'h');
        auto plan = management.call([&] {
            const std::array targets{broadcast::BroadcastTarget(side.endpoint, before),
                                     broadcast::BroadcastTarget(healthy.server.endpoint,
                                                                healthyBinding)};
            return router->route(std::make_shared<const std::string>(healthyPayloads.back()), targets);
        });
        auto queued = dispatcher.dispatch(std::move(plan));
        GAMENET_TEST_ASSERT(queued.scheduledTasks == 2 && queued.scheduledEndpoints == 2);
        transition();
        GAMENET_TEST_ASSERT(!before.isCurrent() && healthyBinding.isCurrent());

        // The healthy leg must complete independently while the stale leg and
        // the old logic command have no opportunity to execute.
        eventually([&] { return healthy.server.snapshot().acknowledged == healthyPayloads.size(); });
        eventually([&] {
            return owners.remote.call([&] { return healthy.client.received == healthyPayloads; });
        });
        const auto progress = queued.progress->snapshot();
        GAMENET_TEST_ASSERT(progress.acceptedEndpoints == 1 && !progress.droppedEndpoints &&
                            progress.outstandingTasks == 1 && !progress.complete);
        GAMENET_TEST_ASSERT(logic->queueSnapshot().depth == 1);

        blockedNetwork.open();
        complete(queued, 1, Reason::OfflineSession);
        ++stats.staleBroadcast;
        blockedLogic.open();
        eventually([&] { return logic->queueSnapshot().droppedStale == stats.staleBroadcast; });
        empty(dispatcher);
    };
    const auto refresh = [&](Side &side, Status expected) {
        auto next = authenticateOn(management, *manager, "churn", side.endpoint, expected);
        GAMENET_TEST_ASSERT(next.isCurrent() && next.sessionId() == binding.sessionId() &&
                            next.generation() > binding.generation());
        binding = next;
    };
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        revokeQueued(old.server, binding, [&] {
            refresh(old.server, Status::Existing);
            GAMENET_TEST_ASSERT(old.server.endpoint->isOpen());
        });
        const auto dropsBefore = relay.drops();
        eventually([&] { return relay.drops() > dropsBefore; });
        expectPending();
    }
    const auto oldBinding = binding;
    revokeQueued(old.server, oldBinding, [&] {
        refresh(replacement.server, Status::Rebound);
        GAMENET_TEST_ASSERT(binding.transportId() != oldBinding.transportId());
        GAMENET_TEST_ASSERT(!old.server.endpoint->isOpen() && replacement.server.endpoint->isOpen());
    });
    eventually([&] {
        const auto s = old.server.snapshot();
        return s.closed == 1 && s.cancelled == 1;
    });
    std::promise<void> lateOffline;
    auto offline = lateOffline.get_future();
    owners.impaired.call([&] {
        const auto refused = old.server.session->send("old-generation");
        GAMENET_TEST_ASSERT(refused.result == kcp::KcpResult::Stopped && !refused.id);
        GAMENET_TEST_ASSERT(old.server.closeReason == Close::GoingAway);
        GAMENET_TEST_ASSERT(manager->postOffline(oldBinding.transportId(), [&](auto result) {
            GAMENET_TEST_ASSERT(management.executor.isInOwnerThread());
            GAMENET_TEST_ASSERT(result == DispatchResult::PolicyRejected);
            auto current = manager->findByTransport(binding.transportId());
            GAMENET_TEST_ASSERT(current && current->binding().generation() == binding.generation());
            GAMENET_TEST_ASSERT(!manager->findByTransport(oldBinding.transportId()));
            lateOffline.set_value();
        }) == DispatchResult::Accepted);
    });
    ready(offline);

    owners.replacement.call([&, liveBinding = binding] {
        replacement.server.input = [&, liveBinding](std::string bytes) {
            GAMENET_TEST_ASSERT(logic->submit(command(liveBinding, std::move(bytes))) ==
                                game_logic::SubmitResult::Accepted);
        };
    });
    const auto inputId = replacement.send(livePayload);
    complete(ready(output), 1);
    eventually([&] { return replacement.server.snapshot().acknowledged == 1; });
    eventually([&] { return replacement.client.snapshot().acknowledged == 1; });
    eventually([&] {
        return owners.remote.call([&] {
            return replacement.client.received == std::vector<std::string>{livePayload};
        });
    });

    revokeQueued(replacement.server, binding, [&] {
        management.call([&] {
            GAMENET_TEST_ASSERT(manager->offline(binding.transportId()));
            GAMENET_TEST_ASSERT(!manager->findByPlayer("churn") && manager->size() == 1);
            GAMENET_TEST_ASSERT(replacement.server.endpoint->requestClose(transport::CloseReason::Normal) ==
                                DispatchResult::Accepted);
        });
        GAMENET_TEST_ASSERT(!replacement.server.endpoint->isOpen());
    });
    eventually([&] { return replacement.server.snapshot().closed == 1; });
    owners.replacement.call([&] {
        GAMENET_TEST_ASSERT(replacement.server.closeReason == Close::Normal);
        GAMENET_TEST_ASSERT(replacement.server.received == std::vector<std::string>{livePayload});
        replacement.server.input = {};
    });

    logicOwner.call([&] {
        const auto dropped = logic->stop();
        const auto s = logic->queueSnapshot();
        GAMENET_TEST_ASSERT(handled == 1 && outputs == 1 && s.accepted == 5 &&
                            !dropped.droppedCommands && !dropped.droppedBytes &&
                            !s.depth && !s.queuedBytes && !s.accepting &&
                            s.droppedStale == 4 && !s.droppedOnStop);
        stats.staleLogic = s.droppedStale;
        logic.reset();
    });
    dispatcher.shutdown();
    empty(dispatcher);
    management.call([&] {
        manager->shutdown();
        GAMENET_TEST_ASSERT(!manager->size() && !manager->findByTransport(healthyBinding.transportId()));
        manager.reset();
        router.reset();
    });
    GAMENET_TEST_ASSERT(!healthyBinding.isCurrent());

    for (const auto &link : {old.stop(), replacement.stop(), healthy.stop()}) {
        for (const auto &s : link) {
            stats.accepted += s.accepted;
            stats.acknowledged += s.acknowledged;
            stats.cancelled += s.cancelled;
            stats.failed += s.failed;
        }
    }
    relay.stop();
    old.server.expectReceipts({{unacknowledged.size(), Send::Cancelled, unacknowledgedId}});
    old.client.expectReceipts({});
    replacement.client.expectReceipts({{livePayload.size(), Send::Acknowledged, inputId}});
    replacement.server.expectReceipts({{livePayload.size(), Send::Acknowledged}});
    healthy.server.expectReceipts({{healthyPayloads[0].size(), Send::Acknowledged},
                                  {healthyPayloads[1].size(), Send::Acknowledged},
                                  {healthyPayloads[2].size(), Send::Acknowledged},
                                  {healthyPayloads[3].size(), Send::Acknowledged}});
    healthy.client.expectReceipts({});
    owners.remote.call([&] {
        GAMENET_TEST_ASSERT(old.client.received == std::vector<std::string>{unacknowledged});
        GAMENET_TEST_ASSERT(replacement.client.received == std::vector<std::string>{livePayload});
        GAMENET_TEST_ASSERT(healthy.client.received == healthyPayloads);
        stats.healthyReceived = healthy.client.received.size();
    });
    stats.ackDrops = relay.drops();
    GAMENET_TEST_ASSERT(stats.ackDrops >= 3 && stats.staleBroadcast == 4 &&
                        stats.healthyReceived == 4 && stats.accepted == 7 &&
                        stats.acknowledged == 6 && stats.cancelled == 1 && !stats.failed);
    owners.checkDrained();
    return stats;
}

void runChurn(std::chrono::seconds minimumDuration) {
    ChurnOwners owners;
    std::uint32_t conv = 1000;
    std::size_t rounds = 0;
    const auto start = base::now();
    do {
        const bool v6 = rounds % 2 != 0;
        const auto s = churnRound(owners, v6, conv);
        ++rounds;
        std::cout << "KCP_C3_CHURN round=" << rounds << " family=" << (v6 ? 6 : 4)
                  << " ack_drops=" << s.ackDrops << " stale_broadcast=" << s.staleBroadcast
                  << " stale_logic=" << s.staleLogic << " healthy_received=" << s.healthyReceived
                  << " accepted=" << s.accepted << " acknowledged=" << s.acknowledged
                  << " cancelled=" << s.cancelled << " failed=" << s.failed
                  << " residual=0" << std::endl;
    } while (rounds < 2 || base::now() - start < minimumDuration);
    if (minimumDuration > 0s) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(base::now() - start);
        std::cout << "KCP_C3_SOAK elapsed_ms=" << elapsed.count() << " rounds=" << rounds << std::endl;
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 1) {
        runChurn(0s);
        run(false);
        run(true);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--churn-soak") {
        runChurn(30min);
        return 0;
    }
    std::cerr << "usage: " << argv[0] << " [--churn-soak]\n";
    return 2;
}
