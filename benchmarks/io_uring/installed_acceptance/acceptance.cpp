// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include <gamenet/core/net/Buffer.h>
#include <gamenet/core/net/EventLoop.h>
#include <gamenet/core/net/TcpConnection.h>
#include <gamenet/core/net/TcpServer.h>
#include <gamenet/experimental/io_uring/IoUringTcpServer.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NDEBUG
#error "The installed consumer must be built in Release"
#endif

namespace {

namespace net = gamenet::net;
namespace ring = gamenet::experimental::io_uring;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Count = std::uint64_t;

constexpr unsigned peers = 64;
constexpr unsigned frameBytes = 4096;
constexpr unsigned warmRounds = 16;
constexpr unsigned echoRounds = 256;
constexpr unsigned burstFrames = 2048;
constexpr auto slowHold = 250ms;
constexpr std::size_t hardLimit = burstFrames * frameBytes;
constexpr std::size_t lowWater = 32U * 1024U;
constexpr std::size_t highWater = 64U * 1024U;
using Frame = std::array<char, frameBytes>;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

Count now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

Count cpu(clockid_t clock) {
    timespec ts{};
    require(::clock_gettime(clock, &ts) == 0, "CPU clock failed");
    return static_cast<Count>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

Count statusValue(std::string_view key) {
    std::ifstream input("/proc/self/status");
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with(key)) return std::stoull(line.substr(key.size()));
    }
    throw std::runtime_error("missing /proc/self/status field: " + std::string(key));
}

Count descriptorCount() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator());
}

int pinnedCpu() {
    cpu_set_t mask{};
    require(::sched_getaffinity(0, sizeof(mask), &mask) == 0 && CPU_COUNT(&mask) == 1,
            "thread affinity is not a single CPU");
    return ::sched_getcpu();
}

void pin(int number) {
    cpu_set_t mask{};
    CPU_ZERO(&mask);
    CPU_SET(number, &mask);
    require(::sched_setaffinity(0, sizeof(mask), &mask) == 0, "pin thread");
    require(pinnedCpu() == number, "verify thread affinity");
}

struct Identity {
    char kind{};
    std::uint32_t peer{};
    std::uint32_t sequence{};
};

Frame makeFrame(Identity id) {
    Frame frame{};
    frame.fill(static_cast<char>('a' + (id.peer + id.sequence) % 23));
    frame[0] = id.kind;
    std::memcpy(frame.data() + 1, &id.peer, sizeof(id.peer));
    std::memcpy(frame.data() + 5, &id.sequence, sizeof(id.sequence));
    return frame;
}

Identity identity(const Frame& frame) {
    Identity id{.kind = frame[0]};
    std::memcpy(&id.peer, frame.data() + 1, sizeof(id.peer));
    std::memcpy(&id.sequence, frame.data() + 5, sizeof(id.sequence));
    return id;
}

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) ::close(fd_); }
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    int fd() const { return fd_; }
private:
    int fd_;
};

Socket connectPeer(unsigned port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "create peer socket");
    Socket socket(fd);
    const int one = 1;
    const int buffer = 64 * 1024;
    const timeval deadline{.tv_sec = 12, .tv_usec = 0};
    require(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0 &&
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) == 0 &&
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)) == 0 &&
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline)) == 0,
            "configure peer socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    require(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "connect peer");
    return socket;
}

void sendFrame(int fd, const Frame& frame) {
    std::size_t offset = 0;
    while (offset != frame.size()) {
        const auto sent = ::send(fd, frame.data() + offset, frame.size() - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        require(sent > 0, "peer send failed or timed out");
        offset += static_cast<std::size_t>(sent);
    }
}

Count receiveFrame(int fd, Identity id) {
    Frame frame{};
    std::size_t offset = 0;
    while (offset != frame.size()) {
        const auto received = ::recv(fd, frame.data() + offset, frame.size() - offset, 0);
        if (received < 0 && errno == EINTR) continue;
        require(received > 0, "peer receive failed, timed out, or saw premature EOF");
        offset += static_cast<std::size_t>(received);
    }
    const auto end = now();
    require(frame == makeFrame(id), "received frame identity or bytes differ");
    return end;
}

Count receiveEofAndHalfClose(int fd) {
    char byte{};
    ssize_t result{};
    do {
        result = ::recv(fd, &byte, 1, 0);
    } while (result < 0 && errno == EINTR);
    require(result == 0, "peer did not see clean EOF after the scheduled frames");
    const Count eof = now();
    require(::shutdown(fd, SHUT_WR) == 0, "return peer half-close");
    return eof;
}

template<class F>
auto onOwner(net::EventLoop& loop, F function) {
    using R = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::move(function));
    auto future = task->get_future();
    require(loop.tryQueueInLoop([task] { (*task)(); }), "post owner control");
    require(future.wait_for(8s) == std::future_status::ready, "owner control timed out");
    return future.get();
}

struct Send {
    Identity id;
    Count admittedNs;
};

struct Receive {
    Identity id;
    Count startNs;
    Count receivedNs;
};

struct Close {
    unsigned peer;
    Count closedNs;
    std::size_t pending;
    int reason;
    int nativeError;
};

struct Result {
    std::map<std::string, Count> metrics;
    std::vector<Send> sent;
    std::vector<Receive> received;
    std::vector<Close> closed;
    std::array<Count, peers> eof{};
    std::array<std::shared_future<ring::IoUringTcpConnectionAdapterStopSummary>, peers> native;
};

template<bool uring>
class Consumer {
    using Server = std::conditional_t<uring, ring::IoUringTcpServer, net::TcpServer>;
    using Connection = std::conditional_t<uring, ring::IoUringTcpConnectionAdapter,
                                          net::TcpConnection>;
    struct State {
        Frame input{};
        std::size_t size{};
        unsigned peer{peers};
    };

public:
    Consumer(net::EventLoop& loop, Result& result) : loop_(loop), result_(result) {
        if constexpr (uring) {
            ring::IoUringTcpServerOptions options;
            options.reusePort = false;
            options.hub.maxConnections = peers;
            options.hub.maxTotalPendingSendBytes = 2 * hardLimit;
            options.hub.maxPendingSendBytesPerConnection = hardLimit;
            options.hub.maxPendingSendSegmentsPerConnection = burstFrames;
            options.hub.maxReceiveBytes = options.hub.maxSendBytesPerOperation = frameBytes;
            options.hub.pump.engine.entries = options.hub.pump.engine.maxOperations = 256;
            options.connection.lowWaterMarkBytes = lowWater;
            options.connection.highWaterMarkBytes = highWater;
            options.connection.hardLimitBytes = hardLimit;
            server_ = std::make_unique<Server>(&loop, net::InetAddress(0, true), options);
            server_->setConnectionCallback([this](Connection& c) {
                if (c.connected()) connected(c);
            });
            server_->setMessageCallback(
                [this](Connection& c, std::string_view bytes) { message(c, bytes); });
            server_->setCloseInfoCallback(
                [this](Connection& c, const net::TcpConnectionCloseInfo& info) { closed(c, info); });
        } else {
            server_ = std::make_unique<Server>(&loop, net::InetAddress(0, true), "installed-epoll", false);
            server_->setThreadNum(0);
            server_->setAdmissionOptions({.maxConnections = peers});
            server_->setConnectionBackpressureOptions({
                .lowWaterMarkBytes = lowWater, .highWaterMarkBytes = highWater,
                .hardLimitBytes = hardLimit, .maxInputBufferBytes = 64U * 1024U});
            auto budget = net::TcpServerOutputMemoryOptions{};
            budget.loop.hardLimitBytes = budget.server.hardLimitBytes = 2 * hardLimit;
            budget.loop.recoveryThresholdBytes = budget.server.recoveryThresholdBytes = hardLimit;
            server_->setOutputMemoryOptions(budget);
            server_->setConnectionCallback([this](const net::TcpConnectionPtr& c) {
                if (c->connected()) connected(*c);
            });
            server_->setMessageCallback([this](const net::TcpConnectionPtr& c, net::Buffer* input) {
                message(*c, std::string_view(input->peek(), input->readableBytes()));
                input->retrieveAll();
            });
            server_->setCloseInfoCallback(
                [this](const net::TcpConnectionPtr& c, const net::TcpConnectionCloseInfo& info) {
                    require(c->socketClosed(), "epoll connection fd still open at close callback");
                    closed(*c, info);
                });
        }
        state_.reserve(peers);
    }

    unsigned start() {
        if constexpr (uring) {
            require(server_->start().result == ring::IoUringTcpServerStartResult::Accepted,
                    "installed io_uring server failed to start");
        } else {
            server_->start();
        }
        return server_->listenAddress().port();
    }

    void burst() {
        require(state_.size() == peers && byPeer_[0], "missing slow peer");
        Connection& connection = *byPeer_[0];
        for (unsigned seq = 0; seq < burstFrames; ++seq) {
            const Identity id{'B', 0, seq};
            send(connection, id, makeFrame(id));
        }
        result_.metrics["slow_pending_at_stop_bytes"] = connection.pendingOutputBytes();
        require(connection.pendingOutputBytes() >= highWater &&
                    connection.readingPausedByBackpressure(), "slow peer did not reach real output pressure");
    }

    auto stop() { return server_->stopGracefully(); }

    template<class Summary>
    void summarize(const Summary& summary) {
        require(server_->connectionCount() == 0 && state_.empty() &&
                    result_.closed.size() == peers, "server connection or callback residue");
        auto& m = result_.metrics;
        if constexpr (uring) {
            const auto& hub = summary.hub;
            const auto& engine = hub.pump.engine;
            require(summary.listenerStoppedBeforeHub && summary.allAdaptersRetired &&
                        hub.allConnectionsStopped && hub.listener && hub.listener->socketClosed &&
                        hub.listener->acceptsRetired, "native server drain was incomplete");
            require(hub.pump.result == ring::IoUringEventLoopPumpStopResult::Drained &&
                        !summary.server.forceEscalated, "native server forced settlement");
            m["connections_accepted"] = hub.hub.connectionsAccepted;
            m["connections_retired"] = hub.hub.connectionsRetired;
            m["terminal_socket_closes"] = hub.hub.socketCloseCount;
            m["send_bytes_discarded"] = hub.hub.bytesDiscarded;
            m["native_operations_accepted"] = engine.operationsAccepted;
            m["native_terminal_notices"] = engine.terminalNotices;
            m["native_notices_dispatched"] = hub.pump.pump.noticesDispatched;
            m["native_stale_cqes"] = engine.staleCqes;
            m["native_fallbacks"] = engine.crossDomainFallbacks;
            m["native_timeouts"] = engine.shutdownTimeouts;
            m["native_cancel_cqes"] = engine.cancelCqes;
            m["native_cancelled_operations"] = engine.cancelledOperations;
            m["native_accept_submissions"] = hub.listener->listener.acceptSubmissions;
            m["native_accept_terminals"] = hub.listener->listener.acceptTerminals;
            m["native_listener_socket_closes"] = hub.listener->listener.listenerSocketCloseCount;
            m["native_callback_or_drive_failures"] = summary.server.callbackFailures +
                hub.hub.invariantFailures + hub.hub.callbackFailures +
                hub.pump.pump.consumerFailures + hub.pump.pump.driveFailures;
            m["final_active_operations"] = engine.activeOperations;
            m["final_pending_submissions"] = engine.pendingSubmissions;
            m["final_pending_cancel_cqes"] = engine.pendingCancelCompletions;
            m["final_pending_notices"] = engine.readyNotices;
            m["final_owned_bytes"] = engine.ownedBytes;
            m["final_active_routes"] = hub.hub.activeOperationRoutes;
            m["final_listener_accepts"] = hub.listener->listener.activeAccepts;
            m["final_active_connections"] =
                summary.server.activeConnections + summary.server.provisionalConnections +
                hub.hub.activeConnections;
            m["final_pending_output_bytes"] = hub.hub.pendingSendBytes;
        } else {
            require(summary.outcome == net::TcpServerStopOutcome::Drained &&
                        summary.initialConnectionCount == peers && !summary.forcedConnectionCount,
                    "epoll server stop was not a complete graceful drain");
            const auto budget = server_->outputMemoryStats();
            m["connections_accepted"] = server_->admissionStats().accepted;
            m["connections_retired"] = result_.closed.size();
            m["final_active_connections"] = server_->connectionCount();
            m["final_pending_output_bytes"] = budget.server.pendingBytes;
            for (const auto& loop : budget.loops) m["final_pending_output_bytes"] += loop.pendingBytes;
        }
    }

private:
    void connected(Connection& c) {
        require(loop_.isInLoopThread() && state_.size() < peers, "unexpected connection owner or count");
        require(state_.try_emplace(&c).second, "duplicate connection callback");
    }

    void send(Connection& c, Identity id, const Frame& frame) {
        const auto at = now();
        require(c.trySend(std::string_view(frame.data(), frame.size())) == net::TcpSendResult::Accepted,
                "send admission unexpectedly rejected");
        result_.sent.push_back({id, at});
    }

    void message(Connection& c, std::string_view bytes) {
        require(loop_.isInLoopThread(), "unexpected message owner");
        auto& state = state_.at(&c);
        while (!bytes.empty()) {
            const auto count = std::min(bytes.size(), frameBytes - state.size);
            std::memcpy(state.input.data() + state.size, bytes.data(), count);
            state.size += count;
            bytes.remove_prefix(count);
            if (state.size != frameBytes) continue;
            const auto id = identity(state.input);
            require(id.peer < peers && (id.kind == 'W' || id.kind == 'E') &&
                        state.input == makeFrame(id), "unexpected input frame");
            if (state.peer == peers) {
                require(!byPeer_[id.peer], "duplicate peer binding");
                byPeer_[id.peer] = &c;
                state.peer = id.peer;
            }
            require(state.peer == id.peer, "input changed peer binding");
            send(c, id, state.input);
            state.size = 0;
        }
    }

    void closed(Connection& c, const net::TcpConnectionCloseInfo& info) {
        require(loop_.isInLoopThread() && c.closePhase() == net::TcpConnectionClosePhase::Closed,
                "unexpected close owner or phase");
        const auto state = state_.find(&c);
        require(state != state_.end() && state->second.peer < peers && !state->second.size,
                "duplicate, unbound or partial-frame close");
        const unsigned peer = state->second.peer;
        require(info.reason == net::TcpConnectionCloseReason::GracefulShutdown &&
                    !info.nativeError && !c.pendingOutputBytes(), "premature or forced connection close");
        if constexpr (uring) result_.native[peer] = c.stopFuture();
        result_.closed.push_back({peer, now(), c.pendingOutputBytes(),
                                  static_cast<int>(info.reason), info.nativeError});
        byPeer_[peer] = nullptr;
        state_.erase(state);
    }

    net::EventLoop& loop_;
    Result& result_;
    std::unique_ptr<Server> server_;
    std::unordered_map<Connection*, State> state_;
    std::array<Connection*, peers> byPeer_{};
};

void echoes(const std::vector<Socket>& sockets, Result& result, char kind, unsigned rounds) {
    std::array<Count, peers> start{};
    for (unsigned seq = 0; seq < rounds; ++seq) {
        for (unsigned peer = 0; peer < peers; ++peer) {
            const auto frame = makeFrame({kind, peer, seq});
            start[peer] = now();
            sendFrame(sockets[peer].fd(), frame);
        }
        for (unsigned peer = 0; peer < peers; ++peer) {
            const Identity id{kind, peer, seq};
            result.received.push_back({id, start[peer], receiveFrame(sockets[peer].fd(), id)});
        }
    }
}

template<bool uring>
void run(bool slow, int peerCpu, Result& result) {
    clockid_t ownerClock{};
    require(::pthread_getcpuclockid(::pthread_self(), &ownerClock) == 0, "owner CPU clock");
    net::EventLoop loop;
    auto consumer = std::make_unique<Consumer<uring>>(loop, result);
    const unsigned port = consumer->start();
    require(port != 0, "server did not publish a bound port");
    result.metrics["owner_cpu"] = pinnedCpu();
    std::jthread client([&] {
        pin(peerCpu);
        auto& m = result.metrics;
        m["peer_cpu"] = pinnedCpu();
        m["threads_during_work"] = statusValue("Threads:");
        require(m["threads_during_work"] == 2, "unexpected thread topology");
        std::vector<Socket> sockets;
        sockets.reserve(peers);
        for (unsigned i = 0; i < peers; ++i) sockets.push_back(connectPeer(port));
        echoes(sockets, result, 'W', warmRounds);
        m["rss_before_measure_kib"] = statusValue("VmRSS:");
        const auto ownerStart = cpu(ownerClock);
        const auto processStart = cpu(CLOCK_PROCESS_CPUTIME_ID);
        m["measure_begin_ns"] = now();
        if (!slow) {
            echoes(sockets, result, 'E', echoRounds);
            m["measure_end_ns"] = now();
            m["owner_cpu_ns"] = cpu(ownerClock) - ownerStart;
            m["process_cpu_ns"] = cpu(CLOCK_PROCESS_CPUTIME_ID) - processStart;
        }
        auto [stopRequested, done] = onOwner(loop, [&] {
            if (slow) consumer->burst();
            return std::pair{now(), consumer->stop()};
        });
        m["stop_requested_ns"] = stopRequested;
        if (slow) {
            std::this_thread::sleep_for(slowHold);
            m["slow_read_released_ns"] = now();
            for (unsigned seq = 0; seq < burstFrames; ++seq) {
                const Identity id{'B', 0, seq};
                result.received.push_back({id, 0, receiveFrame(sockets[0].fd(), id)});
            }
        }
        for (unsigned peer = 0; peer < peers; ++peer) {
            result.eof[peer] = receiveEofAndHalfClose(sockets[peer].fd());
        }
        require(done.wait_for(8s) == std::future_status::ready, "server stop future timed out");
        m["stop_completed_ns"] = now();
        if (slow) {
            m["measure_end_ns"] = m["stop_completed_ns"];
            m["owner_cpu_ns"] = cpu(ownerClock) - ownerStart;
            m["process_cpu_ns"] = cpu(CLOCK_PROCESS_CPUTIME_ID) - processStart;
        }
        m["rss_after_drain_kib"] = statusValue("VmRSS:");
        sockets.clear();
        onOwner(loop, [&] {
            consumer->summarize(done.get());
            consumer.reset();
        });
        loop.quit();
    });
    loop.loop();
    client.join();
    require(loop.phase() == net::EventLoopPhase::Shutdown, "owner loop did not settle");
    result.metrics["final_loop_functors"] = loop.pendingFunctorCount();
    result.metrics["final_loop_controls"] = loop.pendingControlSourceCount();
    result.metrics["final_loop_lifecycle"] =
        loop.attachedLifecycleNodeCount() + loop.pendingLifecycleNodeCount();
    result.metrics["final_loop_callback_errors"] = loop.callbackExceptionCount();
}

void save(const std::filesystem::path& output, Result& result, bool uring) {
    std::ofstream accepted(output / "accepted.csv");
    std::ofstream received(output / "received.csv");
    std::ofstream closed(output / "closed.csv");
    std::ofstream metrics(output / "metrics.csv");
    for (auto* stream : {&accepted, &received, &closed, &metrics}) {
        stream->exceptions(std::ios::badbit | std::ios::failbit);
    }
    accepted << "kind,peer,sequence,accepted_ns\n";
    received << "kind,peer,sequence,start_ns,received_ns\n";
    closed << "peer,eof_ns,closed_ns,pending_bytes,reason,native_error\n";
    for (const auto& a : result.sent)
        accepted << a.id.kind << ',' << a.id.peer << ',' << a.id.sequence << ',' << a.admittedNs << '\n';
    for (const auto& r : result.received)
        received << r.id.kind << ',' << r.id.peer << ',' << r.id.sequence << ',' << r.startNs << ','
                 << r.receivedNs << '\n';
    for (const auto& c : result.closed)
        closed << c.peer << ',' << result.eof[c.peer] << ',' << c.closedNs << ',' << c.pending << ','
               << c.reason << ',' << c.nativeError << '\n';
    for (const auto& [key, value] : result.metrics) metrics << key << ',' << value << '\n';
    if (uring) {
        std::ofstream native(output / "native-connections.csv");
        native.exceptions(std::ios::badbit | std::ios::failbit);
        native << "peer,receive_submissions,receive_terminals,send_submissions,send_terminals,"
                  "send_admissions,bytes_sent,bytes_discarded,pending_bytes,pending_segments,"
                  "pending_commands,socket_closed\n";
        for (unsigned peer = 0; peer < peers; ++peer) {
            const auto& f = result.native[peer];
            require(f.valid() && f.wait_for(0ms) == std::future_status::ready,
                    "native connection settlement unavailable");
            const auto& summary = f.get();
            const auto& c = summary.transport.connection;
            native << peer << ',' << c.receiveSubmissions << ',' << c.receiveTerminals << ','
                   << c.sendSubmissions << ',' << c.sendTerminals << ',' << c.sendAdmissions << ','
                   << c.bytesSent << ',' << c.bytesDiscarded << ',' << c.pendingSendBytes << ','
                   << c.pendingSendSegments << ',' << summary.adapter.pendingCommands << ','
                   << summary.transport.socketClosed << '\n';
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "usage: installed_io_uring_acceptance epoll|io_uring steady|slow-close OUTPUT\n";
        return 2;
    }
    const std::string_view backend(argv[1]), scenario(argv[2]);
    require(backend == "epoll" || backend == "io_uring", "invalid backend");
    require(scenario == "steady" || scenario == "slow-close", "invalid scenario");
    cpu_set_t mask{};
    require(::sched_getaffinity(0, sizeof(mask), &mask) == 0 && CPU_COUNT(&mask) == 2,
            "acceptance runner requires two CPUs in its affinity mask");
    std::vector<int> cores;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &mask)) cores.push_back(i);
    pin(cores[0]);
    Result result;
    result.metrics["fd_before"] = descriptorCount();
    result.metrics["slow_pending_at_stop_bytes"] = 0;
    result.metrics["slow_read_released_ns"] = 0;
    result.metrics["graceful_close_reason"] = static_cast<int>(net::TcpConnectionCloseReason::GracefulShutdown);
    result.sent.reserve(peers * (warmRounds + echoRounds) + burstFrames);
    result.received.reserve(result.sent.capacity());
    result.closed.reserve(peers);
    const bool slow = scenario == "slow-close";
    const bool uring = backend == "io_uring";
    if (uring) run<true>(slow, cores[1], result);
    else run<false>(slow, cores[1], result);
    result.metrics["fd_after"] = descriptorCount();
    result.metrics["threads_after"] = statusValue("Threads:");
    result.metrics["rss_final_kib"] = statusValue("VmRSS:");
    result.metrics["rss_peak_kib"] = statusValue("VmHWM:");
    require(result.metrics["fd_after"] == result.metrics["fd_before"] &&
                result.metrics["threads_after"] == 1, "descriptor or thread residue");
    save(argv[3], result, uring);
}
