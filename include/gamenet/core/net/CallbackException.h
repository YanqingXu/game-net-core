#pragma once

// Exception records for callbacks dispatched by the Reactor/TCP core.
// Policies observe captured exceptions synchronously on the owning EventLoop.

#include <exception>
#include <functional>

namespace gamenet::net {

class TcpConnection;

enum class EventLoopCallbackSource {
    ChannelEvent,
    Timer,
    PendingFunctor,
    Metric,
};

enum class EventLoopCallbackExceptionAction {
    Continue,
    Quit,
};

struct EventLoopCallbackException {
    EventLoopCallbackSource source{EventLoopCallbackSource::PendingFunctor};
    std::exception_ptr exception;
};

using EventLoopCallbackExceptionHandler =
    std::function<EventLoopCallbackExceptionAction(const EventLoopCallbackException&)>;

enum class TcpConnectionCallbackSource {
    Established,
    Disconnected,
    Message,
    HighWaterMark,
    WriteComplete,
    Close,
};

struct TcpConnectionCallbackException {
    TcpConnectionCallbackSource source{TcpConnectionCallbackSource::Message};
    std::exception_ptr exception;
};

using TcpConnectionCallbackExceptionHandler = std::function<void(
    const TcpConnection&,
    const TcpConnectionCallbackException&)>;

}  // namespace gamenet::net
