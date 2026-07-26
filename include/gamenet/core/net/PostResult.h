#pragma once

namespace gamenet::net {

// Typed admission result shared by bounded EventLoop scheduling facades.
enum class PostResult {
    Accepted,
    QueueFull,
    OwnerUnavailable,
    Shutdown,
};

}  // namespace gamenet::net
