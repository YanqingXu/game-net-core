#include "runtime_profiles/MultiIoShardedHybrid.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/InetAddress.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::uint16_t parsePort(int argc, char* argv[]) {
    if (argc <= 1) return 7004;
    try {
        if (argc > 2) throw std::invalid_argument("too many arguments");
        const std::string portText(argv[1]);
        std::size_t consumed = 0;
        const int port = std::stoi(portText, &consumed);
        if (consumed != portText.size() || port <= 0 || port > 65535) {
            throw std::out_of_range("invalid port");
        }
        return static_cast<std::uint16_t>(port);
    } catch (const std::exception&) {
        std::cerr << "usage: multi_io_sharded_hybrid_echo [port]\n";
        std::exit(2);
    }
}

gamenet::examples::MultiIoShardedHybridRoute routePayload(
    gamenet::transport::TransportSessionId,
    std::string_view payload) {
    const auto first = payload.find('|');
    const auto second = first == std::string_view::npos
        ? std::string_view::npos
        : payload.find('|', first + 1);
    const auto third = second == std::string_view::npos
        ? std::string_view::npos
        : payload.find('|', second + 1);
    if (first != 1 || second == std::string_view::npos ||
        third == std::string_view::npos || third + 1 >= payload.size()) {
        throw std::invalid_argument("expected lane|kind|key|payload");
    }
    if (payload.front() != 'e' && payload.front() != 'f') {
        throw std::invalid_argument("lane must be e or f");
    }
    gamenet::examples::LogicShardKeyKind kind{};
    switch (payload[first + 1]) {
    case 'p': kind = gamenet::examples::LogicShardKeyKind::Player; break;
    case 'r': kind = gamenet::examples::LogicShardKeyKind::Room; break;
    case 's': kind = gamenet::examples::LogicShardKeyKind::Scene; break;
    default: throw std::invalid_argument("key kind must be p, r, or s");
    }
    return {
        .key = {
            .kind = kind,
            .value = std::string(payload.substr(second + 1, third - second - 1)),
        },
        .lane = payload.front() == 'f'
            ? gamenet::examples::HybridDispatchLane::FixedTick
            : gamenet::examples::HybridDispatchLane::EventDriven,
    };
}

}  // namespace

int main(int argc, char* argv[]) {
    gamenet::net::EventLoopThread firstLogicThread;
    gamenet::net::EventLoopThread secondLogicThread;
    auto* firstLogicLoop = firstLogicThread.startLoop();
    auto* secondLogicLoop = secondLogicThread.startLoop();
    {
        gamenet::net::EventLoop baseLoop;
        gamenet::examples::MultiIoShardedHybrid profile(
            &baseLoop,
            std::vector<gamenet::net::EventLoop*>{
                firstLogicLoop,
                secondLogicLoop,
            },
            gamenet::net::InetAddress(parsePort(argc, argv)),
            routePayload,
            [](const gamenet::examples::MultiIoShardedHybridContext&,
               gamenet::transport::TransportSessionId,
               std::string_view payload) {
                return gamenet::examples::MultiIoShardedHybridHandlerResult{
                    .reply = std::string(payload),
                };
            });
        profile.start();
        std::cout << "multi_io_sharded_hybrid_echo listening on "
                  << profile.listenAddress().toIpPort()
                  << " (send framed e|p|player-key|payload or "
                     "f|r|room-key|payload)\n";
        baseLoop.loop();
    }
    secondLogicThread.stop();
    firstLogicThread.stop();
    return 0;
}
