// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Derived from intents/usecases/kcp_capacity_endurance.intent.md.
// Development instrument only. Every fixture and callback belongs to this loop.
#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/kcp/KcpTransport.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace gamenet;
using namespace gamenet::net;
using namespace gamenet::udp;
using namespace gamenet::kcp;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::pair<std::size_t, std::size_t> rss() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS info{};
  require(GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)) != 0,
          "RSS query");
  return {info.WorkingSetSize, info.PeakWorkingSetSize};
#else
  std::ifstream in("/proc/self/status");
  std::string key, line;
  std::size_t current = 0, peak = 0, value;
  while (std::getline(in, line)) {
    if (line.starts_with("VmRSS:") || line.starts_with("VmHWM:")) {
      std::istringstream row(line);
      row >> key >> value;
      (key == "VmRSS:" ? current : peak) = value * 1024;
    }
  }
  require(current && peak >= current, "RSS query");
  return {current, peak};
#endif
}
int pid() {
#ifdef _WIN32
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}
void emit(const std::string &type, std::map<std::string, std::uint64_t> fields,
          std::string extra = {}) {
  auto [current, peak] = rss();
  fields["pid"] = pid();
  fields["rss_bytes"] = current;
  fields["peak_rss_bytes"] = peak;
  std::cout << "{\"type\":\"" << type << "\"";
  for (auto &[name, value] : fields)
    std::cout << ",\"" << name << "\":" << value;
  std::cout << extra << "}" << std::endl;
}
bool ack(std::string_view packet) {
  // Inspect standard KCP headers only; forward the original datagram unchanged.
  for (std::size_t offset = 0; offset + 24 <= packet.size();) {
    if (static_cast<unsigned char>(packet[offset + 4]) == 82)
      return true;
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i)
      length |=
          std::uint32_t(static_cast<unsigned char>(packet[offset + 20 + i]))
          << (8 * i);
    if (length > packet.size() - offset - 24)
      break;
    offset += 24 + length;
  }
  return false;
}
struct Ledger {
  std::unordered_set<std::uint64_t> pending;
  std::uint64_t accepted{}, acknowledged{}, cancelled{}, failed{};
  void admit(KcpSendAdmission a) {
    require(a.result == KcpResult::Accepted && a.id &&
                pending.insert(a.id).second,
            "send admission/duplicate id");
    ++accepted;
  }
  void complete(KcpSendCompletion c) {
    require(pending.erase(c.id) == 1, "missing/duplicate local terminal");
    if (c.status == KcpSendStatus::Acknowledged)
      ++acknowledged;
    else if (c.status == KcpSendStatus::Cancelled)
      ++cancelled;
    else
      ++failed;
  }
};
UdpOptions socketOptions(std::size_t count = 4) {
  UdpOptions o;
  o.maxPendingDatagrams = count;
  o.maxPendingBytes = count * 1200;
  o.maxReceiveDatagramsPerBatch = o.maxSendDatagramsPerBatch = 4;
  o.maxReceiveBytesPerBatch = o.maxSendBytesPerBatch = 4800;
  return o;
}
KcpOptions options(std::size_t n) {
  KcpOptions o;
  o.maxSessions = n;
  o.maxSessionOpenings = 2 * n;
  o.maxProtocolBytes = n * 1048576;
  o.maxMessageBytes = 4096;
  o.maxPendingMessagesPerSession = 2;
  o.maxPendingMessages = 2 * n;
  o.maxPendingBytesPerSession = 4096;
  o.maxPendingBytes = 4096 * n;
  o.maxSessionsPerTurn = std::min<std::size_t>(4, n);
  o.maxMessagesPerSessionTurn = 2;
  o.sendTimeout = 5s;
  o.idleTimeout = 60s;
  o.maxTransmissions = 100;
  o.udp.maxPeers = n;
  o.udp.maxPeersPerTurn = std::min<std::size_t>(4, n);
  o.udp.maxPendingDatagramsPerPeer = 4;
  o.udp.maxPendingBytesPerPeer = 4800;
  o.udp.socket = socketOptions(4 * n);
  o.udp.idleTimeout = 60s;
  o.validate();
  return o;
}
bool zero(const UdpSnapshot &s) {
  return s.phase == UdpPhase::Stopped && !s.pendingDatagrams &&
         !s.pendingBytes && !s.nativeOperations && !s.receiveStorageBytes &&
         !s.callbackExceptions &&
         s.acceptedDatagrams ==
             s.sentDatagrams + s.cancelledDatagrams + s.failedDatagrams;
}
bool zero(const KcpSnapshot &s) {
  return s.phase == UdpPhase::Stopped && !s.sessions && !s.pendingMessages &&
         !s.pendingBytes && !s.protocolBytes && !s.scheduledPosts &&
         !s.callbackExceptions && s.opened == s.closed &&
         s.accepted == s.acknowledged + s.cancelled + s.failed &&
         !s.udp.peers && !s.udp.pendingDatagrams && !s.udp.pendingBytes &&
         !s.udp.scheduledPosts && !s.udp.callbackExceptions &&
         zero(s.udp.socket);
}
struct Proxy {
  std::shared_ptr<UdpSocket> front, back;
  bool blocked{true}, droppedAck{};
};
class Cycle {
public:
  EventLoop &loop;
  std::size_t n;
  InetAddress local;
  std::shared_ptr<KcpTransport> hub, memory;
  std::vector<std::shared_ptr<KcpTransport>> peers;
  std::vector<std::shared_ptr<KcpSession>> hs, ps;
  std::shared_ptr<KcpSession> stale;
  std::shared_ptr<UdpSocket> overflow;
  std::vector<Proxy> proxies;
  Ledger sent;
  std::vector<Ledger> echoed;
  std::vector<std::vector<std::string>> expected;
  std::vector<std::size_t> received, peerReceived;
  std::map<std::uint32_t, std::size_t> identities;
  std::map<std::string, std::uint64_t> stats;
  std::uint32_t &nextConv;
  std::string failure;
  std::size_t arena{};
  enum class Phase {
    Broken,
    Healthy,
    Recover,
    Timeout,
    CloseOne,
    RecoverTwo,
    CloseTwo,
    Memory,
    Drain,
    Done
  };
  Phase phase{Phase::Broken};
  Clock::time_point phaseStart{Clock::now()};
  bool stopping{}, healthy{};

  Cycle(EventLoop &l, std::size_t count, int family, std::uint32_t &conv)
      : loop(l), n(count), local(family == 6 ? InetAddress("::1", 0, true)
                                             : InetAddress("127.0.0.1", 0)),
        peers(n), hs(n), ps(n), proxies(n), echoed(n), expected(n), received(n),
        peerReceived(n), nextConv(conv) {}
  void begin() {
    overflow = UdpSocket::create(&loop, local, {}, socketOptions());
    require(overflow->start() == UdpResult::Accepted, "overflow socket start");
    KcpCallbacks callbacks;
    callbacks.message = [this](auto session, auto bytes) {
      guarded([&] {
        const auto i = identities.at(session->identity().conv);
        require(received[i] < expected[i].size() &&
                    expected[i][received[i]] == bytes,
                "hub payload/order/duplicate");
        ++received[i];
      });
    };
    callbacks.sendComplete = [this](auto, auto c) {
      guarded([&] { sent.complete(c); });
    };
    hub = KcpTransport::create(&loop, local, callbacks, options(n));
    require(hub->start() == KcpResult::Accepted, "hub start");
    for (std::size_t i = 0; i < n; ++i) {
      callbacks.message = [this, i](auto session, auto bytes) {
        guarded([&] {
          require(peerReceived[i] < expected[i].size() &&
                      expected[i][peerReceived[i]] == bytes,
                  "peer payload/order/duplicate");
          ++peerReceived[i];
          echoed[i].admit(session->send(bytes));
        });
      };
      callbacks.sendComplete = [this, i](auto, auto c) {
        guarded([&] { echoed[i].complete(c); });
      };
      peers[i] = KcpTransport::create(&loop, local, callbacks, options(1));
      require(peers[i]->start() == KcpResult::Accepted, "peer start");
      UdpCallbacks fc, bc;
      fc.datagram = [this, i](auto &, auto bytes, bool truncated) {
        guarded([&] {
          require(!truncated, "front proxy truncation");
          if (proxies[i].blocked) {
            ++stats["wire_drops"];
            return;
          }
          forward(proxies[i].back, peers[i]->localAddress(), bytes);
        });
      };
      bc.datagram = [this, i](auto &, auto bytes, bool truncated) {
        guarded([&] {
          require(!truncated, "back proxy truncation");
          if (proxies[i].blocked) {
            ++stats["wire_drops"];
            return;
          }
          if (!proxies[i].droppedAck && ack(bytes)) {
            proxies[i].droppedAck = true;
            ++stats["ack_drops"];
            return;
          }
          forward(proxies[i].front, hub->localAddress(), bytes);
        });
      };
      proxies[i].front = UdpSocket::create(&loop, local, fc, socketOptions());
      proxies[i].back = UdpSocket::create(&loop, local, bc, socketOptions());
      require(proxies[i].front->start() == UdpResult::Accepted &&
                  proxies[i].back->start() == UdpResult::Accepted,
              "proxy start");
    }
    openGeneration();
    arena = hub->snapshot().protocolBytes / n;
    require(arena > 0 && hub->snapshot().protocolBytes == arena * n,
            "arena measurement");
    rejection(hub->openSession(overflow->localAddress(), fresh()),
              KcpResult::Overloaded);
    ++stats["session_rejections"];
    sendGeneration();
    for (auto &s : hs) {
      const auto a = s->send("over capacity");
      require(a.result == KcpResult::Overloaded && !a.id,
              "pending capacity rejection");
      ++stats["pending_rejections"];
    }
    observe();
    require(hub->snapshot().pendingMessages == 2 * n,
            "pending pressure not observed");
  }
  template <class F> void guarded(F action) {
    try {
      action();
    } catch (const std::exception &e) {
      failure = e.what();
    }
  }
  std::uint32_t fresh() {
    require(nextConv > 0 && nextConv < 0xfffffff0u, "conv space exhausted");
    return nextConv++;
  }
  void rejection(const KcpSessionAdmission &a, KcpResult result) {
    require(a.result == result && !a.session, "session rejection contract");
  }
  void forward(const std::shared_ptr<UdpSocket> &s, const InetAddress &to,
               std::string_view bytes) {
    const auto a = s->sendTo(to, bytes);
    require(a.result == UdpResult::Accepted ||
                a.result == UdpResult::Overloaded,
            "proxy forwarding");
    if (a.result == UdpResult::Overloaded) {
      require(!a.id, "proxy refusal id");
      ++stats["wire_drops"];
    }
    stats["proxy_peak_datagrams"] = std::max<std::uint64_t>(
        stats["proxy_peak_datagrams"], s->snapshot().pendingDatagrams);
  }
  void openGeneration() {
    for (std::size_t i = 0; i < n; ++i) {
      auto conv = fresh();
      auto h = hub->openSession(proxies[i].front->localAddress(), conv);
      auto p = peers[i]->openSession(proxies[i].back->localAddress(), conv);
      require(h.result == KcpResult::Accepted &&
                  p.result == KcpResult::Accepted,
              "open generation");
      hs[i] = h.session;
      ps[i] = p.session;
      identities[conv] = i;
    }
  }
  void sendGeneration() {
    for (std::size_t i = 0; i < n; ++i) {
      expected[i].push_back("");
      expected[i].push_back(std::string(2048, char('a' + i % 26)));
      sent.admit(hs[i]->send(""));
      sent.admit(hs[i]->send(expected[i].back()));
    }
  }
  bool delivered(std::size_t begin = 0) const {
    for (auto i = begin; i < n; ++i)
      if (received[i] != expected[i].size() ||
          peerReceived[i] != expected[i].size() || !echoed[i].pending.empty())
        return false;
    return true;
  }
  void transition(Phase p) {
    phase = p;
    phaseStart = Clock::now();
  }
  void closeSessions() {
    for (auto &s : hs)
      if (s)
        s->close();
    for (auto &s : ps)
      if (s)
        s->close();
  }
  bool closed() const {
    if (hub->snapshot().sessions)
      return false;
    for (auto &p : peers)
      if (p->snapshot().sessions)
        return false;
    return true;
  }
  void observe() {
    auto h = hub->snapshot();
    auto bounds = [](const KcpSnapshot &s, std::size_t count) {
      require(s.maxSessionsVisitedPerTurn <= std::min<std::size_t>(4, count) &&
                  s.maxMessagesPerTurn <= 2 * std::min<std::size_t>(4, count) &&
                  s.udp.maxPeersVisitedPerTurn <=
                      std::min<std::size_t>(4, count) &&
                  s.udp.maxCompletionsPerTurn <= 32,
              "per-turn work budget");
    };
    bounds(h, n);
    std::size_t protocol = h.protocolBytes;
    require(!h.callbackExceptions && !h.udp.callbackExceptions &&
                !h.udp.socket.callbackExceptions,
            "hub callback exception");
    for (auto &p : peers) {
      auto s = p->snapshot();
      protocol += s.protocolBytes;
      bounds(s, 1);
      require(!s.callbackExceptions && !s.udp.callbackExceptions &&
                  !s.udp.socket.callbackExceptions,
              "peer callback exception");
    }
    for (auto &proxy : proxies)
      for (auto &socket : {proxy.front, proxy.back}) {
        auto s = socket->snapshot();
        require(s.pendingDatagrams <= 4 && s.pendingBytes <= 4800 &&
                    s.maxReceiveBatchDatagrams <= 4 &&
                    s.maxReceiveBatchBytes <= 4800 &&
                    s.maxSendBatchDatagrams <= 4 &&
                    s.maxSendBatchBytes <= 4800 && !s.callbackExceptions,
                "proxy storage/work budget");
      }
    stats["peak_protocol_bytes"] =
        std::max<std::uint64_t>(stats["peak_protocol_bytes"], protocol);
    stats["peak_pending_messages"] = std::max<std::uint64_t>(
        stats["peak_pending_messages"], h.pendingMessages);
    stats["peak_pending_bytes"] =
        std::max<std::uint64_t>(stats["peak_pending_bytes"], h.pendingBytes);
    require(protocol <= 2 * n * 1048576 && h.pendingMessages <= 2 * n &&
                h.pendingBytes <= 4096 * n,
            "capacity overshoot");
  }
  void stop() {
    stopping = true;
    if (hub)
      hub->stop();
    if (memory)
      memory->stop();
    for (auto &p : peers)
      if (p)
        p->stop();
    for (auto &p : proxies) {
      if (p.front)
        p.front->stop();
      if (p.back)
        p.back->stop();
    }
    if (overflow)
      overflow->stop();
    transition(Phase::Drain);
  }
  bool drained() const {
    if ((hub && !zero(hub->snapshot())) ||
        (memory && !zero(memory->snapshot())) ||
        (overflow && !zero(overflow->snapshot())))
      return false;
    for (auto &p : peers)
      if (p && !zero(p->snapshot()))
        return false;
    for (auto &p : proxies)
      if ((p.front && !zero(p.front->snapshot())) ||
          (p.back && !zero(p.back->snapshot())))
        return false;
    return loop.attachedLifecycleNodeCount() == 0;
  }
  void tick() {
    require(failure.empty(), failure.c_str());
    require(Clock::now() - phaseStart < 25s, "phase deadline");
    observe();
    switch (phase) {
    case Phase::Broken:
      if (stats["wire_drops"] < n)
        return;
      for (std::size_t i = 1; i < n; ++i)
        proxies[i].blocked = false;
      transition(Phase::Healthy);
      break;
    case Phase::Healthy:
      if (!delivered(1) || hub->snapshot().pendingMessages != 2)
        return;
      proxies[0].blocked = false;
      transition(Phase::Recover);
      break;
    case Phase::Recover:
      if (!delivered() || !sent.pending.empty())
        return;
      require(stats["ack_drops"] == n, "ACK impairment missing");
      proxies[0].blocked = true;
      sent.admit(hs[0]->send("unreachable"));
      if (n > 1) {
        expected[1].push_back("healthy during failure");
        sent.admit(hs[1]->send(expected[1].back()));
      }
      transition(Phase::Timeout);
      break;
    case Phase::Timeout:
      if (n > 1 && !healthy && received[1] == expected[1].size() &&
          echoed[1].pending.empty()) {
        require(hub->snapshot().pendingMessages >= 1 && hs[0]->isOpen(),
                "healthy work did not overlap blackhole");
        healthy = true;
        stats["healthy_progress"] = 1;
      }
      if (sent.failed != 1)
        return;
      require(n == 1 || healthy, "healthy peer starved");
      stale = hs[0];
      closeSessions();
      transition(Phase::CloseOne);
      break;
    case Phase::CloseOne:
      if (!closed())
        return;
      {
        auto a = stale->send("stale");
        require(a.result == KcpResult::Stopped && !a.id, "stale send");
      }
      ++stats["stale_rejections"];
      rejection(hub->openSession(proxies[0].front->localAddress(),
                                 stale->identity().conv),
                KcpResult::InvalidArgument);
      ++stats["stale_rejections"];
      for (auto &p : proxies)
        p.blocked = false;
      openGeneration();
      sendGeneration();
      transition(Phase::RecoverTwo);
      break;
    case Phase::RecoverTwo:
      if (!delivered() || !sent.pending.empty())
        return;
      proxies[0].blocked = true;
      sent.admit(hs[0]->send("cancel on close"));
      closeSessions();
      require(hub->snapshot().sessions == n,
              "closing sessions released before native retirement");
      stats["closing_charged"] = 1;
      rejection(hub->openSession(overflow->localAddress(), fresh()),
                KcpResult::Overloaded);
      ++stats["session_rejections"];
      transition(Phase::CloseTwo);
      break;
    case Phase::CloseTwo:
      if (!closed())
        return;
      rejection(hub->openSession(overflow->localAddress(), fresh()),
                KcpResult::Overloaded);
      ++stats["history_rejections"];
      {
        auto o = options(2);
        o.maxProtocolBytes = arena;
        memory = KcpTransport::create(&loop, local, {}, o);
        require(memory->start() == KcpResult::Accepted, "memory probe start");
        require(memory->openSession(proxies[0].front->localAddress(), fresh())
                        .result == KcpResult::Accepted,
                "one-arena probe");
        rejection(memory->openSession(overflow->localAddress(), fresh()),
                  KcpResult::Overloaded);
        require(memory->snapshot().protocolBytes == arena &&
                    memory->snapshot().sessions == 1,
                "memory budget overshoot");
        ++stats["memory_rejections"];
      }
      transition(Phase::Memory);
      break;
    case Phase::Memory:
      stop();
      break;
    case Phase::Drain:
      if (!drained())
        return;
      {
        auto h = hub->snapshot();
        require(h.accepted == sent.accepted &&
                    h.acknowledged == sent.acknowledged &&
                    h.cancelled == sent.cancelled && h.failed == sent.failed &&
                    sent.pending.empty(),
                "hub ledger mismatch");
        stats["accepted"] = h.accepted;
        stats["acknowledged"] = h.acknowledged;
        stats["cancelled"] = h.cancelled;
        stats["failed"] = h.failed;
        stats["received"] = h.received;
        for (std::size_t i = 0; i < n; ++i) {
          auto s = peers[i]->snapshot();
          require(s.accepted == echoed[i].accepted &&
                      s.acknowledged == echoed[i].acknowledged &&
                      !s.cancelled && !s.failed && echoed[i].pending.empty(),
                  "peer ledger mismatch");
          stats["peer_accepted"] += s.accepted;
          stats["peer_acknowledged"] += s.acknowledged;
          stats["peer_received"] += s.received;
        }
        stats["healthy_progress"] = healthy ? 1 : 0;
        stats["arena_bytes"] = arena;
        stats["residual"] = 0;
        transition(Phase::Done);
      }
      break;
    case Phase::Done:
      break;
    }
  }
};
} // namespace
int main(int argc, char **argv) {
  try {
    std::map<std::string, unsigned> args;
    for (int i = 1; i + 1 < argc; i += 2) {
      const std::string key = argv[i], value = argv[i + 1];
      require(!args.contains(key) && !value.empty() &&
                  value.find_first_not_of("0123456789") == std::string::npos,
              "arguments");
      args[key] = static_cast<unsigned>(std::stoul(value));
    }
    require(argc == 9 && args.size() == 4, "four named arguments required");
    const auto n = args.at("--sessions"), family = args.at("--family"),
               duration = args.at("--duration-seconds");
    auto conv = args.at("--conv-base");
    require((n == 1 || n == 8 || n == 32) &&
                (family == 0 || family == 4 || family == 6) && duration >= 2 &&
                duration <= 3600 && conv > 0 && conv <= 0x7fffffffu,
            "argument bounds");
    base::Logger::setLogLevel(base::Logger::WARN);
    EventLoop loop;
    const auto start = Clock::now();
    auto elapsed = [&] {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                start)
              .count());
    };
    emit(
        "ready",
        {{"sessions", n},
         {"family", family},
         {"duration_ms", duration * 1000},
         {"conv_base", conv},
         {"hub_protocol_limit", n * 1048576},
         {"peer_protocol_limit", 1048576},
         {"pending_limit", 2 * n},
         {"pending_byte_limit", 4096 * n},
         {"max_proxy_datagrams", 4}},
#ifdef _WIN32
        ",\"schema\":\"gamenet.kcp_validation.child.v1\",\"backend\":\"iocp\"");
#else
        ",\"schema\":\"gamenet.kcp_validation.child.v1\",\"backend\":"
        "\"epoll\"");
#endif
    std::unique_ptr<Cycle> cycle;
    std::uint64_t completed = 0;
    int cycleFamily = 0;
    bool failed = false;
    auto timer = loop.runEvery(5ms, [&] {
      if (failed) {
        if ((cycle && cycle->drained()) ||
            (!cycle && !loop.attachedLifecycleNodeCount()))
          loop.quit();
        return;
      }
      try {
        if (!cycle) {
          cycleFamily = family ? family : (completed % 2 ? 6 : 4);
          cycle = std::make_unique<Cycle>(loop, n, cycleFamily, conv);
          cycle->begin();
        }
        cycle->tick();
        if (cycle->phase != Cycle::Phase::Done)
          return;
        auto stats = cycle->stats;
        cycle.reset(); // release cycle ledgers and copies before the RSS
                       // observation
        stats["cycle"] = ++completed;
        stats["sessions"] = n;
        stats["family"] = cycleFamily;
        stats["elapsed_ms"] = elapsed();
        emit("heartbeat", stats);
        if (elapsed() >= duration * 1000 && (family || completed >= 2)) {
          emit("final",
               {{"cycles", completed},
                {"elapsed_ms", elapsed()},
                {"residual", 0}},
               ",\"status\":\"passed\"");
          loop.quit();
        }
      } catch (const std::exception &e) {
        // Fail closed. The process supervisor records the checkpoint and
        // kills/joins a child that cannot drain; no stopped/zero certificate is
        // synthesized.
        std::cerr << "KCP-C4 cycle=" << completed + 1 << " error=" << e.what()
                  << std::endl;
        failed = true;
        if (cycle)
          cycle->stop();
      }
    });
    loop.loop();
    loop.cancel(timer);
    if (failed) {
      cycle.reset();
      return 1;
    }
    require(loop.attachedLifecycleNodeCount() == 0, "final lifecycle residual");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "KCP-C4 error=" << e.what() << std::endl;
    return 1;
  }
}
