#pragma once

// Callback types shared by the low-level Reactor components.

#include <functional>

namespace gamenet::net {

class EventLoop;

using ThreadInitCallback = std::function<void(EventLoop*)>;

}  // namespace gamenet::net
