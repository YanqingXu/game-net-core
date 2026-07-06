#pragma once

// Callback types shared by the low-level Reactor/TCP core components.

#include <cstddef>
#include <functional>
#include <memory>

namespace gamenet::net {

class Buffer;
class EventLoop;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, std::size_t)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
using ThreadInitCallback = std::function<void(EventLoop*)>;

}  // namespace gamenet::net
