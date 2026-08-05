#pragma once

// Exception records for callbacks dispatched by the Reactor/TCP core.
// Policies observe captured exceptions synchronously on the owning EventLoop.
// They may re-enter owner-safe lifecycle APIs. If an EventLoop handler throws,
// the loop logs and quits; a TcpConnection observer throw is contained and the
// fixed close/cleanup policy continues.

#include <exception>
#include <functional>

namespace gamenet::net {

class TcpConnection;

enum class EventLoopCallbackSource {
    ChannelEvent,
    Timer,
    PendingFunctor,
    Metric,
    // Appended to preserve the numeric values of the 0.3 compatibility-line
    // callback sources for consumers that persist diagnostics.
    Control,
    Lifecycle,
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
    CloseInfo,
};

struct TcpConnectionCallbackException {
    TcpConnectionCallbackSource source{TcpConnectionCallbackSource::Message};
    std::exception_ptr exception;
};

using TcpConnectionCallbackExceptionHandler = std::function<void(
    const TcpConnection&,
    const TcpConnectionCallbackException&)>;

}  // namespace gamenet::net
