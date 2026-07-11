#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include <iostream>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(7001, true));
    pipeline.start();
    std::cout << "game server pipeline demo listening on "
              << pipeline.listenAddress().toIpPort() << '\n';
    loop.loop();
}
