#pragma once

#include "gamenet/game_session/PlayerSession.h"
#include "gamenet/transport/TransportEndpoint.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace gamenet::game_logic {

enum class CommandPriority : std::uint8_t {
    Low,
    Normal,
    High,
};

struct GameCommand {
    gamenet::game_session::SessionId sessionId{};
    gamenet::transport::TransportSessionId transportId{};
    std::uint32_t messageId{};
    std::uint64_t requestId{};
    CommandPriority priority{CommandPriority::Normal};
    std::string payload;
    std::chrono::steady_clock::time_point enqueuedAt{};
};

}  // namespace gamenet::game_logic
