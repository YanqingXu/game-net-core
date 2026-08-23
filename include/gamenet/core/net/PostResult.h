// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

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
