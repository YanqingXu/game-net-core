#include "IoUringTcpConnectionHub.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include "core/net/detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gamenet::experimental::io_uring {

namespace {

IoUringTcpConnectionHubOptions validateOptions(
    gamenet::net::EventLoop* ownerLoop,
    IoUringTcpConnectionHubOptions options) {
    const auto connections = options.maxConnections;
    const auto maxSize = (std::numeric_limits<std::size_t>::max)();
    const bool operationCountOverflows = connections > maxSize / 2U;
    const bool receiveBudgetOverflows =
        connections != 0 && options.maxReceiveBytes > maxSize / connections;
    const bool perConnectionEngineBudgetOverflows =
        options.maxReceiveBytes >
        maxSize - options.maxSendBytesPerOperation;
    const auto perConnectionEngineBudget = perConnectionEngineBudgetOverflows
        ? maxSize
        : options.maxReceiveBytes + options.maxSendBytesPerOperation;
    const bool aggregateEngineBudgetOverflows =
        connections != 0 &&
        perConnectionEngineBudget > maxSize / connections;

    if (ownerLoop == nullptr || connections < 2 ||
        connections > (std::numeric_limits<std::uint32_t>::max)() ||
        options.maxTotalPendingSendBytes == 0 ||
        options.maxReceiveBytes == 0 ||
        options.maxSendBytesPerOperation == 0 ||
        options.maxPendingSendBytesPerConnection == 0 ||
        options.maxPendingSendSegmentsPerConnection == 0 ||
        options.maxSendBytesPerOperation >
            options.maxPendingSendBytesPerConnection ||
        options.maxReceiveBytes > options.pump.engine.maxBytesPerOperation ||
        options.maxSendBytesPerOperation >
            options.pump.engine.maxBytesPerOperation ||
        operationCountOverflows ||
        options.pump.engine.maxOperations < connections * 2U ||
        receiveBudgetOverflows || perConnectionEngineBudgetOverflows ||
        aggregateEngineBudgetOverflows ||
        options.pump.engine.maxOwnedBytes <
            perConnectionEngineBudget * connections) {
        throw std::invalid_argument(
            "IoUringTcpConnectionHub requires finite two-connection Recv/Send and aggregate budgets");
    }
    return options;
}

std::uint32_t nextGeneration(std::uint32_t current) noexcept {
    ++current;
    if (current == 0) ++current;
    return current;
}

bool sameOperation(
    IoUringOperationIdentity left,
    IoUringOperationIdentity right) noexcept {
    return left == right;
}

}  // namespace

class IoUringTcpConnectionHubImpl final {
public:
    using Identity = IoUringTcpConnectionIdentity;
    using ConnectionMetrics = IoUringTcpConnectionHubConnectionMetrics;

    IoUringTcpConnectionHubImpl(
        gamenet::net::EventLoop* ownerLoop,
        IoUringTcpConnectionHubOptions options,
        IoUringTcpConnectionHub::StoppedConsumer stoppedConsumer)
        : ownerLoop_(ownerLoop),
          options_(validateOptions(ownerLoop, options)),
          stoppedConsumer_(std::move(stoppedConsumer)),
          stopFuture_(stopPromise_.get_future().share()),
          slots_(options_.maxConnections),
          operationRoutes_(options_.pump.engine.maxOperations) {
        ownerLoop_->assertInLoopThread();
    }

    void initialize() {
        assertOwner();
        maintenanceSource_ =
            gamenet::net::detail::EventLoopLifecycleRegistry::attach(
                *ownerLoop_,
                [this] { driveMaintenance(); });
        maintenanceAttached_ = true;
        try {
            pump_ = std::make_unique<IoUringEventLoopPump>(
                ownerLoop_,
                options_.pump,
                [this](IoUringCompletionNotice& notice) {
                    handleNotice(notice);
                },
                [this](const IoUringEventLoopPumpStopSummary& summary) {
                    handlePumpStopped(summary);
                });
        } catch (...) {
            detachMaintenanceNoThrow();
            throw;
        }
    }

    ~IoUringTcpConnectionHubImpl() {
        if (!pump_) return;
        try {
            assertOwner();
            if (phase_ != IoUringTcpHubPhase::Stopped) {
                (void)beginHubStop(IoUringTcpHubCloseReason::Destroyed);
            }
        } catch (...) {
        }
        pump_.reset();
        detachMaintenanceNoThrow();
    }

    IoUringTcpConnectionHubAddOutcome addConnection(
        gamenet::net::SocketFd establishedSocket,
        IoUringTcpConnectionHub::MessageConsumer messageConsumer,
        IoUringTcpConnectionHub::CloseConsumer closeConsumer,
        IoUringTcpConnectionHub::OutputProgressConsumer
            outputProgressConsumer) {
        gamenet::net::Socket transferred(establishedSocket);
        assertOwner();
        if (!gamenet::net::sockets::isValid(establishedSocket) ||
            !messageConsumer) {
            return {.result = IoUringTcpHubAddResult::RejectedInvalid};
        }
        if (phase_ == IoUringTcpHubPhase::Quiescing) {
            return {.result = IoUringTcpHubAddResult::RejectedQuiescing};
        }
        if (phase_ == IoUringTcpHubPhase::Stopped) {
            return {.result = IoUringTcpHubAddResult::RejectedShutdown};
        }

        std::size_t slotIndex = slots_.size();
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].route) {
                slotIndex = index;
                break;
            }
        }
        if (slotIndex == slots_.size()) {
            return {.result = IoUringTcpHubAddResult::ConnectionLimit};
        }

        auto& slot = slots_[slotIndex];
        slot.generation = nextGeneration(slot.generation);
        const Identity identity{
            .slot = static_cast<std::uint32_t>(slotIndex),
            .generation = slot.generation,
        };
        slot.route = std::make_unique<Route>(
            identity,
            transferred.releaseFd(),
            std::move(messageConsumer),
            std::move(closeConsumer),
            std::move(outputProgressConsumer));
        auto future = slot.route->stopFuture;

        const auto receiveResult = submitReceive(*slot.route);
        if (receiveResult != IoUringSubmissionResult::Accepted) {
            ++metrics_.engineRejections;
            slot.route->socket.close();
            slot.route.reset();
            if (receiveResult == IoUringSubmissionResult::RejectedQuiescing) {
                (void)beginHubStop(
                    IoUringTcpHubCloseReason::EventLoopQuiescing);
                return {.result = IoUringTcpHubAddResult::RejectedQuiescing};
            }
            if (receiveResult == IoUringSubmissionResult::RejectedShutdown) {
                return {.result = IoUringTcpHubAddResult::RejectedShutdown};
            }
            return {.result = IoUringTcpHubAddResult::EngineRejected};
        }

        ++metrics_.connectionsAccepted;
        ++metrics_.activeConnections;
        metrics_.maxActiveConnections = (std::max)(
            metrics_.maxActiveConnections,
            metrics_.activeConnections);
        return {
            .result = IoUringTcpHubAddResult::Accepted,
            .identity = identity,
            .stopFuture = std::move(future),
        };
    }

    IoUringTcpHubSendResult send(Identity identity, std::string_view payload) {
        assertOwner();
        auto* route = findRoute(identity, true);
        if (route == nullptr) return IoUringTcpHubSendResult::StaleConnection;
        if (route->phase != RoutePhase::Running ||
            phase_ != IoUringTcpHubPhase::Running) {
            return IoUringTcpHubSendResult::Closing;
        }
        if (payload.empty()) return IoUringTcpHubSendResult::EmptyPayload;
        if (route->sendSegments.size() >=
            options_.maxPendingSendSegmentsPerConnection) {
            ++metrics_.connectionSegmentLimitRejections;
            return IoUringTcpHubSendResult::ConnectionSegmentLimit;
        }
        if (payload.size() >
            options_.maxPendingSendBytesPerConnection -
                route->pendingSendBytes) {
            ++metrics_.connectionByteLimitRejections;
            return IoUringTcpHubSendResult::ConnectionByteLimit;
        }
        if (payload.size() >
            options_.maxTotalPendingSendBytes - pendingSendBytes_) {
            ++metrics_.hubByteLimitRejections;
            return IoUringTcpHubSendResult::HubByteLimit;
        }

        route->sendSegments.emplace_back(payload);
        reservePendingBytes(*route, payload.size());
        ++route->metrics.sendAdmissions;
        ++metrics_.sendAdmissions;
        if (!route->sendIdentity.valid()) {
            const auto result = submitFrontSend(*route);
            if (result != IoUringSubmissionResult::Accepted) {
                const auto bytes = route->sendSegments.back().size();
                releasePendingBytes(*route, bytes, false);
                route->sendSegments.pop_back();
                --route->metrics.sendAdmissions;
                --metrics_.sendAdmissions;
                updateRouteQueueMetrics(*route);
                ++metrics_.engineRejections;
                if (result == IoUringSubmissionResult::RejectedQuiescing ||
                    result == IoUringSubmissionResult::RejectedShutdown) {
                    (void)beginHubStop(
                        IoUringTcpHubCloseReason::EventLoopQuiescing);
                } else {
                    (void)beginRouteClose(
                        *route,
                        IoUringTcpHubCloseReason::EngineRejected,
                        true);
                }
                return IoUringTcpHubSendResult::EngineRejected;
            }
        }
        updateRouteQueueMetrics(*route);
        return IoUringTcpHubSendResult::Accepted;
    }

    IoUringTcpHubReadControlResult pauseRead(Identity identity) {
        assertOwner();
        auto* route = findRoute(identity, true);
        if (route == nullptr) {
            return IoUringTcpHubReadControlResult::StaleConnection;
        }
        if (route->phase != RoutePhase::Running ||
            phase_ != IoUringTcpHubPhase::Running) {
            return IoUringTcpHubReadControlResult::Closing;
        }
        if (!route->readDesired) {
            return IoUringTcpHubReadControlResult::Unchanged;
        }
        route->readDesired = false;
        if (route->receiveIdentity.valid() &&
            !requestCancellation(
                *route,
                route->receiveIdentity,
                route->retryReceiveCancel,
                true)) {
            (void)beginRouteClose(
                *route,
                IoUringTcpHubCloseReason::EngineRejected,
                true);
            return IoUringTcpHubReadControlResult::EngineRejected;
        }
        return IoUringTcpHubReadControlResult::Applied;
    }

    IoUringTcpHubReadControlResult resumeRead(Identity identity) {
        assertOwner();
        auto* route = findRoute(identity, true);
        if (route == nullptr) {
            return IoUringTcpHubReadControlResult::StaleConnection;
        }
        if (route->phase != RoutePhase::Running ||
            phase_ != IoUringTcpHubPhase::Running) {
            return IoUringTcpHubReadControlResult::Closing;
        }
        if (route->readDesired) {
            return IoUringTcpHubReadControlResult::Unchanged;
        }
        route->readDesired = true;
        if (!route->receiveIdentity.valid() &&
            submitReceive(*route) != IoUringSubmissionResult::Accepted) {
            ++metrics_.engineRejections;
            (void)beginRouteClose(
                *route,
                IoUringTcpHubCloseReason::EngineRejected,
                true);
            return IoUringTcpHubReadControlResult::EngineRejected;
        }
        return IoUringTcpHubReadControlResult::Applied;
    }

    bool closeConnection(Identity identity, IoUringTcpHubCloseReason reason) {
        assertOwner();
        auto* route = findRoute(identity, true);
        if (route == nullptr) return false;
        return beginRouteClose(*route, reason, true);
    }

    bool stop() {
        assertOwner();
        return beginHubStop(IoUringTcpHubCloseReason::HubStopped);
    }

    IoUringTcpHubPhase phase() const noexcept { return phase_; }

    IoUringTcpConnectionHubMetrics metrics() const noexcept {
        auto snapshot = metrics_;
        snapshot.activeConnections = metrics_.activeConnections;
        snapshot.activeOperationRoutes = activeOperationRoutes_;
        snapshot.pendingSendBytes = pendingSendBytes_;
        snapshot.foreignMutationRejections =
            foreignMutationRejections_.load(std::memory_order_relaxed);
        return snapshot;
    }

    std::shared_future<IoUringTcpConnectionHubStopSummary> stopFuture() const {
        return stopFuture_;
    }

private:
    enum class RoutePhase : std::uint8_t {
        Running,
        Closing,
    };

    struct Route {
        Route(
            Identity value,
            gamenet::net::SocketFd socketFd,
            IoUringTcpConnectionHub::MessageConsumer message,
            IoUringTcpConnectionHub::CloseConsumer close,
            IoUringTcpConnectionHub::OutputProgressConsumer outputProgress)
            : identity(value),
              socket(socketFd),
              messageConsumer(std::move(message)),
              closeConsumer(std::move(close)),
              outputProgressConsumer(std::move(outputProgress)),
              stopFuture(stopPromise.get_future().share()) {}

        Identity identity{};
        gamenet::net::Socket socket;
        IoUringTcpConnectionHub::MessageConsumer messageConsumer;
        IoUringTcpConnectionHub::CloseConsumer closeConsumer;
        IoUringTcpConnectionHub::OutputProgressConsumer
            outputProgressConsumer;
        std::promise<IoUringTcpConnectionHubConnectionStopSummary> stopPromise;
        std::shared_future<IoUringTcpConnectionHubConnectionStopSummary>
            stopFuture;
        std::deque<std::string> sendSegments;
        IoUringOperationIdentity receiveIdentity{};
        IoUringOperationIdentity sendIdentity{};
        ConnectionMetrics metrics{};
        RoutePhase phase{RoutePhase::Running};
        IoUringTcpHubCloseReason closeReason{
            IoUringTcpHubCloseReason::Explicit};
        int closeNativeError{};
        std::size_t pendingSendBytes{};
        bool readDesired{true};
        bool retryReceiveCancel{false};
        bool retrySendCancel{false};
    };

    struct Slot {
        std::uint32_t generation{};
        std::unique_ptr<Route> route;
    };

    struct OperationRoute {
        IoUringOperationIdentity operation{};
        Identity connection{};
        IoUringOperationKind kind{IoUringOperationKind::Receive};
        bool active{false};
    };

    void assertOwner() const {
        if (!ownerLoop_->isInLoopThread()) {
            foreignMutationRejections_.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error(
                "IoUringTcpConnectionHub used from a different thread");
        }
    }

    Route* findRoute(Identity identity, bool countStale) noexcept {
        if (!identity.valid() || identity.slot >= slots_.size()) {
            if (countStale) ++metrics_.staleConnectionRejections;
            return nullptr;
        }
        auto& slot = slots_[identity.slot];
        if (!slot.route || slot.generation != identity.generation) {
            if (countStale) ++metrics_.staleConnectionRejections;
            return nullptr;
        }
        return slot.route.get();
    }

    bool bindOperation(
        Route& route,
        IoUringOperationIdentity operation,
        IoUringOperationKind kind) noexcept {
        if (!operation.valid() || operation.slot >= operationRoutes_.size() ||
            operationRoutes_[operation.slot].active) {
            ++metrics_.invariantFailures;
            return false;
        }
        operationRoutes_[operation.slot] = {
            .operation = operation,
            .connection = route.identity,
            .kind = kind,
            .active = true,
        };
        ++activeOperationRoutes_;
        return true;
    }

    IoUringSubmissionResult submitReceive(Route& route) {
        if (phase_ != IoUringTcpHubPhase::Running ||
            route.phase != RoutePhase::Running || !route.readDesired ||
            route.receiveIdentity.valid()) {
            return IoUringSubmissionResult::Accepted;
        }
        const auto outcome = pump_->enqueueRecv(
            route.socket.fd(),
            options_.maxReceiveBytes);
        if (outcome.result != IoUringSubmissionResult::Accepted) {
            return outcome.result;
        }
        route.receiveIdentity = outcome.identity;
        if (!bindOperation(
                route,
                outcome.identity,
                IoUringOperationKind::Receive)) {
            (void)pump_->cancel(outcome.identity);
            return IoUringSubmissionResult::RejectedInvalid;
        }
        ++route.metrics.receiveSubmissions;
        route.metrics.maxActiveReceives = 1;
        return IoUringSubmissionResult::Accepted;
    }

    IoUringSubmissionResult submitFrontSend(Route& route) {
        if (route.sendSegments.empty() || route.sendIdentity.valid()) {
            return IoUringSubmissionResult::Accepted;
        }
        const auto& front = route.sendSegments.front();
        const auto size =
            (std::min)(front.size(), options_.maxSendBytesPerOperation);
        const auto outcome = pump_->enqueueSend(
            route.socket.fd(),
            std::string_view(front.data(), size));
        if (outcome.result != IoUringSubmissionResult::Accepted) {
            return outcome.result;
        }
        route.sendIdentity = outcome.identity;
        if (!bindOperation(route, outcome.identity, IoUringOperationKind::Send)) {
            (void)pump_->cancel(outcome.identity);
            return IoUringSubmissionResult::RejectedInvalid;
        }
        ++route.metrics.sendSubmissions;
        route.metrics.maxActiveSends = 1;
        return IoUringSubmissionResult::Accepted;
    }

    bool requestCancellation(
        Route& route,
        IoUringOperationIdentity identity,
        bool& retry,
        bool receive) {
        if (!identity.valid()) {
            retry = false;
            return true;
        }
        const auto result = pump_->cancel(identity);
        switch (result) {
        case IoUringCancelResult::Accepted:
        case IoUringCancelResult::AlreadyRequested:
            retry = false;
            if (receive) ++route.metrics.receiveCancelRequests;
            return true;
        case IoUringCancelResult::SubmissionQueueFull:
            retry = true;
            signalMaintenance();
            return true;
        case IoUringCancelResult::RejectedInvalid:
            // Engine releases the active operation before its terminal notice
            // is dispatched. The still-active Hub route entry proves that
            // this exact generation is already terminal and needs no cancel.
            retry = false;
            return true;
        case IoUringCancelResult::RejectedNotSubmitted:
            retry = true;
            signalMaintenance();
            return true;
        case IoUringCancelResult::RejectedShutdown:
            retry = false;
            ++metrics_.engineRejections;
            return phase_ != IoUringTcpHubPhase::Running;
        }
        return false;
    }

    bool beginRouteClose(
        Route& route,
        IoUringTcpHubCloseReason reason,
        bool requestLocalCancellations,
        int nativeError = 0) {
        if (route.phase == RoutePhase::Closing) {
            if (route.closeNativeError == 0 && nativeError != 0) {
                route.closeNativeError = nativeError;
            }
            return false;
        }
        route.phase = RoutePhase::Closing;
        route.closeReason = reason;
        route.closeNativeError = nativeError;
        route.readDesired = false;
        ++route.metrics.closeRequests;
        discardQueuedSendSegmentsAfterActive(route);
        if (requestLocalCancellations &&
            phase_ == IoUringTcpHubPhase::Running) {
            (void)requestCancellation(
                route,
                route.receiveIdentity,
                route.retryReceiveCancel,
                true);
            (void)requestCancellation(
                route,
                route.sendIdentity,
                route.retrySendCancel,
                false);
        }
        if (!route.receiveIdentity.valid() &&
            !route.sendIdentity.valid()) {
            finalizeReadyRoutes();
        }
        return true;
    }

    bool beginHubStop(IoUringTcpHubCloseReason reason) {
        if (phase_ != IoUringTcpHubPhase::Running) return false;
        phase_ = IoUringTcpHubPhase::Quiescing;
        hubCloseReason_ = reason;
        for (auto& slot : slots_) {
            if (slot.route) {
                (void)beginRouteClose(*slot.route, reason, false);
            }
        }
        pump_->beginQuiesce();
        return true;
    }

    void handleNotice(IoUringCompletionNotice& notice) noexcept {
        ++consumerDepth_;
        metrics_.maxConsumerDepth =
            (std::max)(metrics_.maxConsumerDepth, consumerDepth_);
        if (ownerLoop_->phase() != gamenet::net::EventLoopPhase::Running &&
            phase_ == IoUringTcpHubPhase::Running) {
            (void)beginHubStop(
                IoUringTcpHubCloseReason::EventLoopQuiescing);
        }
        try {
            dispatchNotice(notice);
        } catch (...) {
            ++metrics_.callbackFailures;
            (void)beginHubStop(IoUringTcpHubCloseReason::EngineRejected);
        }
        --consumerDepth_;
        if (consumerDepth_ == 0) finalizeReadyRoutes();
    }

    void dispatchNotice(IoUringCompletionNotice& notice) {
        const auto operation = notice.identity();
        if (!operation.valid() || operation.slot >= operationRoutes_.size()) {
            recordRoutingFailure();
            return;
        }
        auto& entry = operationRoutes_[operation.slot];
        if (!entry.active || !sameOperation(entry.operation, operation) ||
            entry.kind != notice.kind()) {
            recordRoutingFailure();
            return;
        }
        const auto connection = entry.connection;
        entry = {};
        --activeOperationRoutes_;
        auto* route = findRoute(connection, false);
        if (route == nullptr) {
            recordRoutingFailure();
            return;
        }

        if (notice.kind() == IoUringOperationKind::Receive) {
            if (!sameOperation(route->receiveIdentity, operation)) {
                recordRoutingFailure();
                return;
            }
            route->receiveIdentity = {};
            route->retryReceiveCancel = false;
            handleReceiveNotice(*route, notice);
            return;
        }
        if (notice.kind() == IoUringOperationKind::Send) {
            if (!sameOperation(route->sendIdentity, operation)) {
                recordRoutingFailure();
                return;
            }
            route->sendIdentity = {};
            route->retrySendCancel = false;
            handleSendNotice(*route, notice);
            return;
        }
        recordRoutingFailure();
    }

    void recordRoutingFailure() noexcept {
        ++metrics_.invariantFailures;
        (void)beginHubStop(IoUringTcpHubCloseReason::EngineRejected);
    }

    void handleReceiveNotice(
        Route& route,
        IoUringCompletionNotice& notice) {
        ++route.metrics.receiveTerminals;
        if (notice.status() == IoUringCompletionStatus::Cancelled) {
            ++route.metrics.receiveCancellations;
            if (route.phase == RoutePhase::Running && route.readDesired &&
                submitReceive(route) != IoUringSubmissionResult::Accepted) {
                ++metrics_.engineRejections;
                (void)beginRouteClose(
                    route,
                    IoUringTcpHubCloseReason::EngineRejected,
                    true);
            }
            return;
        }
        if (route.phase == RoutePhase::Closing) return;
        if (notice.status() != IoUringCompletionStatus::Succeeded) {
            (void)beginRouteClose(
                route,
                IoUringTcpHubCloseReason::ReceiveFailed,
                true,
                notice.nativeError());
            return;
        }
        if (notice.bytesTransferred() == 0) {
            (void)beginRouteClose(
                route,
                IoUringTcpHubCloseReason::PeerClosed,
                true);
            return;
        }
        if (notice.bytesTransferred() != notice.payload().size()) {
            ++metrics_.invariantFailures;
            (void)beginRouteClose(
                route,
                IoUringTcpHubCloseReason::EngineRejected,
                true);
            return;
        }

        ++route.metrics.messagesDelivered;
        route.metrics.bytesReceived += notice.bytesTransferred();
        ++metrics_.messagesDelivered;
        metrics_.bytesReceived += notice.bytesTransferred();
        try {
            route.messageConsumer(route.identity, notice.payload());
        } catch (...) {
            ++route.metrics.callbackFailures;
            ++metrics_.callbackFailures;
            (void)beginRouteClose(
                route,
                IoUringTcpHubCloseReason::CallbackFailed,
                true);
            return;
        }
        if (phase_ == IoUringTcpHubPhase::Running &&
            route.phase == RoutePhase::Running && route.readDesired &&
            !route.receiveIdentity.valid() &&
            submitReceive(route) != IoUringSubmissionResult::Accepted) {
            ++metrics_.engineRejections;
            (void)beginRouteClose(
                route,
                IoUringTcpHubCloseReason::EngineRejected,
                true);
        }
    }

    void handleSendNotice(Route& route, IoUringCompletionNotice& notice) {
        ++route.metrics.sendTerminals;
        if (route.sendSegments.empty()) {
            recordRoutingFailure();
            return;
        }
        auto& front = route.sendSegments.front();
        const auto submittedBytes =
            (std::min)(front.size(), options_.maxSendBytesPerOperation);
        const std::string_view expected(front.data(), submittedBytes);
        const bool validSuccess =
            notice.status() == IoUringCompletionStatus::Succeeded &&
            notice.bytesTransferred() != 0 &&
            notice.bytesTransferred() <= submittedBytes &&
            notice.payload() == expected;
        if (!validSuccess) {
            discardFrontSendSegment(route);
            if (route.phase == RoutePhase::Running) {
                (void)beginRouteClose(
                    route,
                    IoUringTcpHubCloseReason::SendFailed,
                    true,
                    notice.nativeError());
            }
            return;
        }

        const auto transferred = notice.bytesTransferred();
        route.metrics.bytesSent += transferred;
        metrics_.bytesSent += transferred;
        releasePendingBytes(route, transferred, false);
        front.erase(0, transferred);
        if (front.empty()) route.sendSegments.pop_front();
        updateRouteQueueMetrics(route);

        if (route.outputProgressConsumer) {
            try {
                route.outputProgressConsumer(
                    route.identity,
                    route.pendingSendBytes);
            } catch (...) {
                ++route.metrics.callbackFailures;
                ++metrics_.callbackFailures;
                (void)beginRouteClose(
                    route,
                    IoUringTcpHubCloseReason::CallbackFailed,
                    true);
            }
        }

        if (route.phase == RoutePhase::Closing) {
            discardAllSendSegments(route);
            return;
        }
        if (!route.sendSegments.empty() &&
            submitFrontSend(route) != IoUringSubmissionResult::Accepted) {
            ++metrics_.engineRejections;
            (void)beginRouteClose(
                route,
                IoUringTcpHubCloseReason::EngineRejected,
                true);
        }
    }

    void reservePendingBytes(Route& route, std::size_t bytes) noexcept {
        route.pendingSendBytes += bytes;
        pendingSendBytes_ += bytes;
        metrics_.pendingSendBytes = pendingSendBytes_;
        metrics_.maxPendingSendBytes =
            (std::max)(metrics_.maxPendingSendBytes, pendingSendBytes_);
    }

    void releasePendingBytes(
        Route& route,
        std::size_t bytes,
        bool discarded) noexcept {
        if (bytes > route.pendingSendBytes || bytes > pendingSendBytes_) {
            ++metrics_.invariantFailures;
            bytes = (std::min)(bytes, route.pendingSendBytes);
            bytes = (std::min)(bytes, pendingSendBytes_);
        }
        route.pendingSendBytes -= bytes;
        pendingSendBytes_ -= bytes;
        if (discarded) {
            route.metrics.bytesDiscarded += bytes;
            metrics_.bytesDiscarded += bytes;
        }
        metrics_.pendingSendBytes = pendingSendBytes_;
    }

    void discardFrontSendSegment(Route& route) noexcept {
        if (route.sendSegments.empty()) return;
        const auto bytes = route.sendSegments.front().size();
        releasePendingBytes(route, bytes, true);
        route.sendSegments.pop_front();
        updateRouteQueueMetrics(route);
    }

    void discardQueuedSendSegmentsAfterActive(Route& route) noexcept {
        const std::size_t retained = route.sendIdentity.valid() ? 1U : 0U;
        while (route.sendSegments.size() > retained) {
            const auto bytes = route.sendSegments.back().size();
            releasePendingBytes(route, bytes, true);
            route.sendSegments.pop_back();
        }
        updateRouteQueueMetrics(route);
    }

    void discardAllSendSegments(Route& route) noexcept {
        while (!route.sendSegments.empty()) {
            discardFrontSendSegment(route);
        }
    }

    void updateRouteQueueMetrics(Route& route) noexcept {
        route.metrics.pendingSendBytes = route.pendingSendBytes;
        route.metrics.pendingSendSegments = route.sendSegments.size();
        route.metrics.maxPendingSendBytes = (std::max)(
            route.metrics.maxPendingSendBytes,
            route.pendingSendBytes);
        route.metrics.maxPendingSendSegments = (std::max)(
            route.metrics.maxPendingSendSegments,
            route.sendSegments.size());
    }

    void finalizeReadyRoutes() {
        if (consumerDepth_ != 0 || finalizing_ ||
            phase_ != IoUringTcpHubPhase::Running) {
            return;
        }
        finalizing_ = true;
        bool progress = true;
        while (progress && phase_ == IoUringTcpHubPhase::Running) {
            progress = false;
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                auto* route = slots_[index].route.get();
                if (route == nullptr || route->phase != RoutePhase::Closing ||
                    route->receiveIdentity.valid() ||
                    route->sendIdentity.valid()) {
                    continue;
                }
                finalizeRoute(index);
                progress = true;
                if (phase_ != IoUringTcpHubPhase::Running) break;
            }
        }
        finalizing_ = false;
    }

    void finalizeRoute(std::size_t index) {
        auto route = std::move(slots_[index].route);
        if (!route) return;
        discardAllSendSegments(*route);
        route->socket.close();
        ++metrics_.socketCloseCount;
        --metrics_.activeConnections;
        ++metrics_.connectionsRetired;
        updateRouteQueueMetrics(*route);
        const auto summary = IoUringTcpConnectionHubConnectionStopSummary{
            .identity = route->identity,
            .reason = route->closeReason,
            .nativeError = route->closeNativeError,
            .connection = route->metrics,
            .socketClosed =
                !gamenet::net::sockets::isValid(route->socket.fd()),
        };
        route->stopPromise.set_value(summary);
        if (route->closeConsumer) {
            try {
                route->closeConsumer(route->identity, route->closeReason);
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
    }

    void signalMaintenance() noexcept {
        if (!maintenanceAttached_) return;
        (void)maintenanceSource_.signal();
    }

    void driveMaintenance() {
        assertOwner();
        if (phase_ != IoUringTcpHubPhase::Running) return;
        bool retryRemains = false;
        for (auto& slot : slots_) {
            auto* route = slot.route.get();
            if (route == nullptr) continue;
            if (route->retryReceiveCancel && route->receiveIdentity.valid()) {
                (void)requestCancellation(
                    *route,
                    route->receiveIdentity,
                    route->retryReceiveCancel,
                    true);
            }
            if (route->retrySendCancel && route->sendIdentity.valid()) {
                (void)requestCancellation(
                    *route,
                    route->sendIdentity,
                    route->retrySendCancel,
                    false);
            }
            retryRemains = retryRemains || route->retryReceiveCancel ||
                route->retrySendCancel;
        }
        if (retryRemains) signalMaintenance();
    }

    void handlePumpStopped(
        const IoUringEventLoopPumpStopSummary& pumpSummary) noexcept {
        if (phase_ == IoUringTcpHubPhase::Running) {
            hubCloseReason_ =
                ownerLoop_->phase() == gamenet::net::EventLoopPhase::Running
                ? IoUringTcpHubCloseReason::EngineRejected
                : IoUringTcpHubCloseReason::EventLoopQuiescing;
            phase_ = IoUringTcpHubPhase::Quiescing;
            for (auto& slot : slots_) {
                if (slot.route) {
                    (void)beginRouteClose(
                        *slot.route,
                        hubCloseReason_,
                        false);
                }
            }
        }

        if (activeOperationRoutes_ != 0) {
            ++metrics_.invariantFailures;
            for (auto& operation : operationRoutes_) operation = {};
            activeOperationRoutes_ = 0;
        }
        for (auto& slot : slots_) {
            if (!slot.route) continue;
            slot.route->receiveIdentity = {};
            slot.route->sendIdentity = {};
            slot.route->retryReceiveCancel = false;
            slot.route->retrySendCancel = false;
        }
        detachMaintenanceNoThrow();
        phase_ = IoUringTcpHubPhase::Stopped;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].route) finalizeRoute(index);
        }

        const auto summary = IoUringTcpConnectionHubStopSummary{
            .hub = metrics(),
            .pump = pumpSummary,
            .allConnectionsStopped = metrics_.activeConnections == 0 &&
                activeOperationRoutes_ == 0 && pendingSendBytes_ == 0,
        };
        if (!stopPublished_) {
            stopPublished_ = true;
            stopPromise_.set_value(summary);
        }
        if (stoppedConsumer_) {
            try {
                stoppedConsumer_(summary);
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
    }

    void detachMaintenanceNoThrow() noexcept {
        if (!maintenanceAttached_) return;
        try {
            gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                *ownerLoop_,
                maintenanceSource_);
        } catch (...) {
        }
        maintenanceAttached_ = false;
    }

    gamenet::net::EventLoop* ownerLoop_;
    IoUringTcpConnectionHubOptions options_;
    IoUringTcpConnectionHub::StoppedConsumer stoppedConsumer_;
    std::promise<IoUringTcpConnectionHubStopSummary> stopPromise_;
    std::shared_future<IoUringTcpConnectionHubStopSummary> stopFuture_;
    std::vector<Slot> slots_;
    std::vector<OperationRoute> operationRoutes_;
    std::unique_ptr<IoUringEventLoopPump> pump_;
    gamenet::net::EventLoopLifecycleSource maintenanceSource_;
    IoUringTcpConnectionHubMetrics metrics_{};
    mutable std::atomic<std::uint64_t> foreignMutationRejections_{0};
    IoUringTcpHubPhase phase_{IoUringTcpHubPhase::Running};
    IoUringTcpHubCloseReason hubCloseReason_{
        IoUringTcpHubCloseReason::HubStopped};
    std::size_t activeOperationRoutes_{};
    std::size_t pendingSendBytes_{};
    std::size_t consumerDepth_{};
    bool maintenanceAttached_{false};
    bool finalizing_{false};
    bool stopPublished_{false};
};

IoUringTcpConnectionHub::IoUringTcpConnectionHub(
    gamenet::net::EventLoop* ownerLoop,
    IoUringTcpConnectionHubOptions options,
    StoppedConsumer stoppedConsumer)
    : impl_(std::make_unique<IoUringTcpConnectionHubImpl>(
          ownerLoop,
          options,
          std::move(stoppedConsumer))) {
    impl_->initialize();
}

IoUringTcpConnectionHub::~IoUringTcpConnectionHub() = default;

IoUringTcpConnectionHubAddOutcome IoUringTcpConnectionHub::addConnection(
    gamenet::net::SocketFd establishedSocket,
    MessageConsumer messageConsumer,
    CloseConsumer closeConsumer,
    OutputProgressConsumer outputProgressConsumer) {
    return impl_->addConnection(
        establishedSocket,
        std::move(messageConsumer),
        std::move(closeConsumer),
        std::move(outputProgressConsumer));
}

IoUringTcpHubSendResult IoUringTcpConnectionHub::send(
    IoUringTcpConnectionIdentity connection,
    std::string_view payload) {
    return impl_->send(connection, payload);
}

IoUringTcpHubReadControlResult IoUringTcpConnectionHub::pauseRead(
    IoUringTcpConnectionIdentity connection) {
    return impl_->pauseRead(connection);
}

IoUringTcpHubReadControlResult IoUringTcpConnectionHub::resumeRead(
    IoUringTcpConnectionIdentity connection) {
    return impl_->resumeRead(connection);
}

bool IoUringTcpConnectionHub::closeConnection(
    IoUringTcpConnectionIdentity connection,
    IoUringTcpHubCloseReason reason) {
    return impl_->closeConnection(connection, reason);
}

bool IoUringTcpConnectionHub::stop() { return impl_->stop(); }

IoUringTcpHubPhase IoUringTcpConnectionHub::phase() const noexcept {
    return impl_->phase();
}

IoUringTcpConnectionHubMetrics IoUringTcpConnectionHub::metrics() const noexcept {
    return impl_->metrics();
}

std::shared_future<IoUringTcpConnectionHubStopSummary>
IoUringTcpConnectionHub::stopFuture() const {
    return impl_->stopFuture();
}

}  // namespace gamenet::experimental::io_uring
