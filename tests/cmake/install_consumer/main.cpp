#include <gamenet/core/net/Buffer.h>
#include <gamenet/core/net/InetAddress.h>
#include <gamenet/core/metrics/MetricsHookRecorder.h>
#include <gamenet/broadcast/BroadcastMetricsRecorder.h>
#include <gamenet/broadcast/BroadcastTypes.h>
#include <gamenet/game_logic/GameCommandQueue.h>
#include <gamenet/game_logic/LogicMetricsRecorder.h>
#include <gamenet/game_session/PlayerSession.h>
#include <gamenet/protocol/PacketFramer.h>
#include <gamenet/transport/TcpTransportEndpoint.h>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

class InstalledEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    gamenet::transport::TransportSessionId id() const noexcept override { return {17}; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override { return {}; }
    gamenet::transport::EndpointResult send(std::string_view bytes) override {
        return open_ && bytes == "installed"
                   ? gamenet::transport::EndpointResult::Accepted
                   : gamenet::transport::EndpointResult::Closed;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_ = false;
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_; }

private:
    bool open_{true};
};

}  // namespace

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
    auto metrics = std::make_shared<gamenet::metrics::InMemoryMetricsExporter>();
    auto connectorMetrics = gamenet::metrics::MetricsHookRecorder(metrics).makeConnectorCallback();
    connectorMetrics(address, gamenet::net::ConnectorEvent::ConnectSuccess);
    auto logicMetrics = gamenet::game_logic::makeLogicMetricsCallback(metrics);
    logicMetrics({});
    auto broadcastMetrics = gamenet::broadcast::makeBroadcastMetricsCallback(metrics);
    broadcastMetrics(metric);
    auto endpoint = std::make_shared<InstalledEndpoint>();
    gamenet::game_session::PlayerSession session(
        23,
        "installed-player",
        endpoint,
        gamenet::game_session::PlayerSession::Clock::now());
    session.markOnline(gamenet::game_session::PlayerSession::Clock::now());

    bool rejectedNullTcpConnection = false;
    try {
        gamenet::transport::TcpTransportEndpoint invalidTcpEndpoint({99}, {});
        (void)invalidTcpEndpoint;
    } catch (const std::invalid_argument&) {
        rejectedNullTcpConnection = true;
    }

    if (buffer.readableBytes() != 2 || address.port() != 0 || !frame ||
        queue.submit(std::move(command)) != gamenet::game_logic::SubmitResult::Accepted ||
        metric.reason != gamenet::broadcast::BroadcastReason::None ||
        session.state() != gamenet::game_session::SessionState::Online ||
        session.transportId().value != 17 ||
        endpoint->send("installed") != gamenet::transport::EndpointResult::Accepted ||
        metrics->counterValue("gamenet.net.connector.connect_success") != 1 ||
        metrics->counterValue("gamenet.game_logic.tick_completed") != 1 ||
        metrics->counterValue("gamenet.broadcast.event.routed") != 1 ||
        !rejectedNullTcpConnection) {
        return 1;
    }

    return 0;
}
