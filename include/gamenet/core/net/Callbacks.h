#pragma once

// Callback types shared by the low-level Reactor/TCP core components.

#include "gamenet/core/net/TcpConnectionClose.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace gamenet::net {

class Buffer;
class EventLoop;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
// Connection callbacks execute synchronously on the connection owner loop and
// may re-enter connection/server/client lifecycle APIs. The shared connection
// argument may be retained. Buffer* is borrowed only for MessageCallback's
// invocation and must not be retained; its readable storage may be invalidated
// by any callback mutation. Connection/message/high-water/write-complete
// exceptions close only that connection; disconnect/close observer exceptions
// are reported and contained so cleanup continues.
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, std::size_t)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseInfoCallback = std::function<void(
    const TcpConnectionPtr&,
    const TcpConnectionCloseInfo&)>;
// With worker threads, runs on each newly created EventLoop owner before
// publication and exceptions propagate through the EventLoopThread startup
// handshake. EventLoopThreadPool zero-worker mode instead invokes it
// synchronously on the existing base loop; TcpServer/EventLoopThreadPool
// start() propagate that exception directly and roll startup back.
using ThreadInitCallback = std::function<void(EventLoop*)>;

}  // namespace gamenet::net
