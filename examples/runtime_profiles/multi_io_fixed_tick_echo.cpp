#include "runtime_profiles/MultiIoDedicatedFixedTick.h"

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

namespace {

std::uint16_t parsePort(int argc, char* argv[]) {
    if (argc <= 1) return 7003;
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
        std::cerr << "usage: multi_io_fixed_tick_echo [port]\n";
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();
    {
        gamenet::net::EventLoop baseLoop;
        gamenet::examples::MultiIoDedicatedFixedTick profile(
            &baseLoop,
            logicLoop,
            gamenet::net::InetAddress(parsePort(argc, argv)),
            [](const gamenet::examples::FixedTickContext&,
               gamenet::transport::TransportSessionId,
               std::string_view payload) {
                return gamenet::examples::MultiIoDedicatedFixedTickHandlerResult{
                    .reply = std::string(payload),
                };
            });
        profile.start();
        std::cout << "multi_io_fixed_tick_echo listening on "
                  << profile.listenAddress().toIpPort()
                  << " (20ms fixed-rate skip-missed cadence)\n";
        baseLoop.loop();
    }
    logicThread.stop();
    return 0;
}
