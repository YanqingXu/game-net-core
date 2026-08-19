#include "runtime_profiles/SingleLoopInlineEvent.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/transport/TcpTransportEndpoint.h"

#include <algorithm>
#include <any>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace gamenet::examples {
namespace {

gamenet::net::EventLoop* requireOwnerLoop(gamenet::net::EventLoop* loop) {
    if (loop == nullptr) {
        throw std::invalid_argument(
            "SingleLoopInlineEvent requires an owner EventLoop");
    }
    return loop;
}

}  // namespace

struct SingleLoopInlineEvent::CallbackState {
    CallbackState(
        SingleLoopInlineHandler handlerValue,
        SingleLoopInlineEventOptions optionsValue)
        : handler(std::move(handlerValue)), options(std::move(optionsValue)) {
        if (!handler ||
            options.maxHandlerWallTime <=
                std::chrono::steady_clock::duration::zero() ||
            options.framing.maxFramesPerPush == 0 ||
            options.framing.maxFrameBytesPerPush == 0) {
            throw std::invalid_argument(
                "SingleLoopInlineEvent requires a handler and positive budgets");
        }
        (void)gamenet::protocol::PacketFramer(options.framing);
        options.admission.validate();
        options.connectionBackpressure.validate();
    }

    bool active() const noexcept { return accepting; }
    void revoke() noexcept { accepting = false; }

    SingleLoopInlineHandler handler;
    SingleLoopInlineEventOptions options;
    SingleLoopInlineEventMetrics metrics;
    std::uint64_t nextTransportId{1};
    bool accepting{true};
};

struct SingleLoopInlineEvent::ConnectionState {
    ConnectionState(
        const gamenet::protocol::PacketFramerOptions& framing,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpointValue,
        std::shared_ptr<CallbackState> callbackStateValue)
        : framer(framing),
          endpoint(std::move(endpointValue)),
          ownerExecutor(endpoint->ownerExecutor()),
          callbackState(std::move(callbackStateValue)) {}

    gamenet::protocol::PacketFramer framer;
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint;
    gamenet::net::EventLoopExecutor ownerExecutor;
    std::shared_ptr<CallbackState> callbackState;
    bool closing{false};
    bool continuationQueued{false};
};

SingleLoopInlineEvent::SingleLoopInlineEvent(
    gamenet::net::EventLoop* loop,
    const gamenet::net::InetAddress& listenAddress,
    SingleLoopInlineHandler handler,
    SingleLoopInlineEventOptions options)
    : loop_(requireOwnerLoop(loop)),
      callbackState_(std::make_shared<CallbackState>(
          std::move(handler), std::move(options))),
      server_(loop_, listenAddress, "single_loop_inline_event") {
    loop_->assertInLoopThread();

    // Profile A has one explicit I/O owner. Adding worker loops changes the
    // Profile and must be reviewed as a different composition.
    server_.setThreadNum(0);
    server_.setAdmissionOptions(callbackState_->options.admission);
    server_.setConnectionBackpressureOptions(
        callbackState_->options.connectionBackpressure);

    const auto callbackState = callbackState_;
    server_.setConnectionCallback(
        [callbackState](const gamenet::net::TcpConnectionPtr& connection) {
            connection->getLoop()->assertInLoopThread();
            if (connection->connected()) {
                if (!callbackState->active()) {
                    connection->forceClose();
                    return;
                }
                auto endpoint =
                    std::make_shared<gamenet::transport::TcpTransportEndpoint>(
                        gamenet::transport::TransportSessionId{
                            callbackState->nextTransportId++},
                        connection);
                auto state = std::make_shared<ConnectionState>(
                    callbackState->options.framing,
                    std::move(endpoint),
                    callbackState);
                connection->setContext(state);
                ++callbackState->metrics.connectionsOpened;
                return;
            }

            const auto* context =
                std::any_cast<std::shared_ptr<ConnectionState>>(
                    &connection->getContext());
            if (context == nullptr || !*context) return;
            (*context)->closing = true;
            (*context)->continuationQueued = false;
            connection->setContext(std::any{});
            ++callbackState->metrics.connectionsClosed;
        });

    server_.setMessageCallback(
        [callbackState](
            const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* input) {
            connection->getLoop()->assertInLoopThread();
            if (!callbackState->active()) {
                input->retrieveAll();
                return;
            }
            const auto* context =
                std::any_cast<std::shared_ptr<ConnectionState>>(
                    &connection->getContext());
            if (context == nullptr || !*context || (*context)->closing) {
                input->retrieveAll();
                return;
            }
            const auto state = *context;
            handleFramerResult(
                state,
                state->framer.push(input->retrieveAllAsString()));
        });
}

SingleLoopInlineEvent::~SingleLoopInlineEvent() {
    if (!stopped_) stop();
    callbackState_->revoke();
}

void SingleLoopInlineEvent::start() {
    loop_->assertInLoopThread();
    if (started_) return;
    if (stopped_) {
        throw std::logic_error(
            "SingleLoopInlineEvent cannot restart after stop");
    }
    server_.start();
    started_ = true;
}

void SingleLoopInlineEvent::stop() {
    (void)stopGracefully(gamenet::net::TcpServerStopOptions{
        .drainTimeout = std::chrono::milliseconds::zero(),
    });
}

gamenet::net::TcpServerStopFuture SingleLoopInlineEvent::stopGracefully(
    gamenet::net::TcpServerStopOptions options) {
    loop_->assertInLoopThread();
    if (stopped_) return stopFuture_;
    callbackState_->revoke();
    started_ = false;
    stopped_ = true;
    stopFuture_ = server_.stopGracefully(options);
    return stopFuture_;
}

const gamenet::net::InetAddress&
SingleLoopInlineEvent::listenAddress() const noexcept {
    return server_.listenAddress();
}

SingleLoopInlineEventMetrics SingleLoopInlineEvent::metrics() const {
    loop_->assertInLoopThread();
    return callbackState_->metrics;
}

void SingleLoopInlineEvent::handleFramerResult(
    const std::shared_ptr<ConnectionState>& state,
    gamenet::protocol::FrameResult result) {
    auto& callbackState = state->callbackState;
    if (state->closing || !callbackState->active()) return;

    const auto close = [state](gamenet::transport::CloseReason reason) {
        if (state->closing) return;
        state->closing = true;
        (void)state->endpoint->close(reason);
    };

    if (result.status == gamenet::protocol::FrameStatus::FrameTooLarge ||
        result.status == gamenet::protocol::FrameStatus::BufferLimitExceeded ||
        result.status == gamenet::protocol::FrameStatus::Faulted) {
        ++callbackState->metrics.protocolFailures;
        close(gamenet::transport::CloseReason::ProtocolError);
        return;
    }

    std::size_t handlersThisDispatch = 0;
    for (const auto& frame : result.frames) {
        if (!callbackState->active() || state->closing) return;
        ++handlersThisDispatch;
        ++callbackState->metrics.handlerCalls;
        callbackState->metrics.maxHandlersPerDispatch = std::max(
            callbackState->metrics.maxHandlersPerDispatch,
            handlersThisDispatch);

        SingleLoopInlineHandlerResult handlerResult;
        const auto startedAt = std::chrono::steady_clock::now();
        try {
            handlerResult = callbackState->handler(
                state->endpoint->id(), frame);
        } catch (...) {
            ++callbackState->metrics.handlerExceptions;
            close(gamenet::transport::CloseReason::GoingAway);
            return;
        }
        const auto handlerDuration =
            std::chrono::steady_clock::now() - startedAt;
        if (handlerDuration > callbackState->options.maxHandlerWallTime) {
            ++callbackState->metrics.handlerOverruns;
            close(gamenet::transport::CloseReason::Overloaded);
            return;
        }

        // stop() is allowed to re-enter from the handler. It revokes admission
        // before returning, so no reply or later frame may escape afterward.
        if (!callbackState->active() || state->closing) return;

        if (handlerResult.reply) {
            const auto encoded = state->framer.encode(*handlerResult.reply);
            if (!encoded) {
                ++callbackState->metrics.protocolFailures;
                close(gamenet::transport::CloseReason::ProtocolError);
                return;
            }
            const auto sendResult = state->endpoint->send(*encoded);
            if (sendResult == gamenet::transport::EndpointResult::Accepted) {
                ++callbackState->metrics.repliesAccepted;
            } else {
                if (sendResult ==
                    gamenet::transport::EndpointResult::Overloaded) {
                    ++callbackState->metrics.outputOverloads;
                    close(gamenet::transport::CloseReason::Overloaded);
                } else {
                    close(gamenet::transport::CloseReason::GoingAway);
                }
                return;
            }
        }

        if (handlerResult.action == SingleLoopInlineAction::Close) {
            close(gamenet::transport::CloseReason::Normal);
            return;
        }
    }

    if (!result.needsContinuation || state->continuationQueued ||
        !callbackState->active() || state->closing) {
        return;
    }

    state->continuationQueued = true;
    const auto posted = state->ownerExecutor.post([state] {
        state->continuationQueued = false;
        if (!state->callbackState->active() || state->closing) return;
        handleFramerResult(state, state->framer.push({}));
    });
    if (posted == gamenet::net::PostResult::Accepted) {
        ++callbackState->metrics.continuationPosts;
        return;
    }

    state->continuationQueued = false;
    ++callbackState->metrics.continuationRejections;
    close(
        posted == gamenet::net::PostResult::QueueFull
            ? gamenet::transport::CloseReason::Overloaded
            : gamenet::transport::CloseReason::GoingAway);
}

}  // namespace gamenet::examples
