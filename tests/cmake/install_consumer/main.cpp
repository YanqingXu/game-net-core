#include <gamenet/core/net/Buffer.h>
#include <gamenet/core/net/InetAddress.h>
#include <gamenet/broadcast/BroadcastTypes.h>
#include <gamenet/game_logic/GameCommandQueue.h>
#include <gamenet/protocol/PacketFramer.h>

#include <utility>

int main() {
    gamenet::net::Buffer buffer;
    buffer.append("ok", 2);

    gamenet::net::InetAddress address(0);
    gamenet::protocol::PacketFramer framer;
    auto frame = framer.encode("installed");
    gamenet::game_logic::GameCommandQueue queue;
    gamenet::game_logic::GameCommand command;
    command.payload = "command";
    gamenet::broadcast::BroadcastMetric metric;
    if (buffer.readableBytes() != 2 || address.port() != 0 || !frame ||
        queue.submit(std::move(command)) != gamenet::game_logic::SubmitResult::Accepted ||
        metric.reason != gamenet::broadcast::BroadcastReason::None) {
        return 1;
    }

    return 0;
}
