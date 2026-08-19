#include "gamenet/core/net/Acceptor.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "gamenet/core/base/Logger.h"

#ifdef _WIN32
#include "gamenet/core/net/platform/IocpOperation.h"
#include "gamenet/core/net/platform/IocpSocketOps.h"
#include "detail/IocpOperationState.h"
#include "detail/NetworkMemoryRetentionTracker.h"
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
#include "detail/AcceptorIocpHarness.h"
#endif

#include <array>
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
#include <atomic>
#endif
#include <vector>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace gamenet::net {

namespace {

[[noreturn]] void throwSocketError(const char* operation, int error) {
    throw std::system_error(
        std::error_code(error, std::system_category()),
        std::string(operation) + ": " + sockets::errorMessage(error));
}

SocketFd createAcceptSocket(sa_family_t family) {
    const SocketFd fd = sockets::createNonblocking(family);
    if (!sockets::isValid(fd)) {
        throwSocketError("accept socket creation", sockets::lastError());
    }
    return fd;
}

constexpr std::size_t kMaxIocpAcceptDepth = 64;

}  // namespace

#ifdef _WIN32

namespace {

constexpr DWORD kAcceptAddressBytes = sizeof(sockaddr_storage) + 16;

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
std::atomic<std::size_t> currentIocpAcceptSlots{0};
std::atomic<std::size_t> peakIocpAcceptSlots{0};
std::atomic<std::size_t> currentIocpAcceptSockets{0};
std::atomic<std::size_t> peakIocpAcceptSockets{0};
std::atomic<std::size_t> currentIocpAcceptSubmissions{0};
std::atomic<std::size_t> peakIocpAcceptSubmissions{0};
std::atomic<std::uint64_t> iocpAcceptSubmissionCount{0};
std::atomic<std::uint64_t> iocpAcceptCompletionCount{0};
std::atomic<std::uint64_t> iocpAcceptCancellationCount{0};
std::atomic<std::uint64_t> maxIocpAcceptGeneration{0};
std::atomic<std::int64_t> iocpAcceptPostsBeforeInjectedFailure{-1};
std::atomic<int> iocpInjectedAcceptError{WSAENOBUFS};

void updatePeak(
    std::atomic<std::size_t>& peak,
    std::size_t value) noexcept {
    std::size_t observed = peak.load(std::memory_order_relaxed);
    while (observed < value &&
           !peak.compare_exchange_weak(
               observed,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void observeSlotPoolCreated(std::size_t depth) noexcept {
    const std::size_t current =
        currentIocpAcceptSlots.fetch_add(depth, std::memory_order_relaxed) +
        depth;
    updatePeak(peakIocpAcceptSlots, current);
}

void observeSlotPoolDestroyed(std::size_t depth) noexcept {
    currentIocpAcceptSlots.fetch_sub(depth, std::memory_order_relaxed);
}

void observeAcceptedSocketCreated() noexcept {
    const std::size_t current =
        currentIocpAcceptSockets.fetch_add(1, std::memory_order_relaxed) + 1;
    updatePeak(peakIocpAcceptSockets, current);
}

void observeAcceptedSocketReleased() noexcept {
    currentIocpAcceptSockets.fetch_sub(1, std::memory_order_relaxed);
}

void observeAcceptSubmitted() noexcept {
    const std::size_t current =
        currentIocpAcceptSubmissions.fetch_add(1, std::memory_order_relaxed) +
        1;
    updatePeak(peakIocpAcceptSubmissions, current);
    iocpAcceptSubmissionCount.fetch_add(1, std::memory_order_relaxed);
}

void observeAcceptCompleted() noexcept {
    currentIocpAcceptSubmissions.fetch_sub(1, std::memory_order_relaxed);
    iocpAcceptCompletionCount.fetch_add(1, std::memory_order_relaxed);
}

bool takeInjectedAcceptError(int* error) noexcept {
    std::int64_t remaining =
        iocpAcceptPostsBeforeInjectedFailure.load(std::memory_order_acquire);
    while (remaining >= 0) {
        if (remaining == 0) {
            if (iocpAcceptPostsBeforeInjectedFailure.compare_exchange_weak(
                    remaining,
                    -1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                *error =
                    iocpInjectedAcceptError.load(std::memory_order_acquire);
                return true;
            }
            continue;
        }
        if (iocpAcceptPostsBeforeInjectedFailure.compare_exchange_weak(
                remaining,
                remaining - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }
    }
    return false;
}
#endif

}  // namespace

struct Acceptor::IocpAcceptSlot {
    IocpOperation operation{};
    IocpAcceptState* state{nullptr};
    SocketFd accepted{kInvalidSocket};
    std::array<char, kAcceptAddressBytes * 2> addresses{};
    std::uint64_t generation{0};
    bool pending{false};
    bool completionPending{false};
    bool cancelling{false};

    IocpAcceptSlot() {
        operation.kind = IocpOperationKind::Accept;
    }
};

struct Acceptor::IocpAcceptState {
    enum class Phase {
        Accepting,
        Retrying,
        Stopped,
    };

    explicit IocpAcceptState(std::size_t depth)
        : slots(depth) {
        for (auto& slot : slots) {
            slot.state = this;
            slot.operation.terminalContext = &slot;
            slot.operation.terminalObserver = +[](
                void* context,
                IocpOperationKind kind) noexcept {
                auto& completed =
                    *static_cast<IocpAcceptSlot*>(context);
                if (kind != IocpOperationKind::Accept ||
                    !completed.pending) {
                    return;
                }
                completed.pending = false;
                completed.completionPending = true;
            };
            slot.operation.completionContext = &slot;
            slot.operation.completionConsumer = +[](
                void* context,
                gamenet::base::Timestamp observedAt,
                bool observerCurrent) {
                auto& completed =
                    *static_cast<IocpAcceptSlot*>(context);
                IocpAcceptState* state = completed.state;
                if (state == nullptr || !completed.completionPending) {
                    return;
                }
                if (!observerCurrent || state->owner == nullptr) {
                    completed.completionPending = false;
                    completed.cancelling = false;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                    observeAcceptCompleted();
#endif
                    if (sockets::isValid(completed.accepted)) {
                        sockets::close(completed.accepted);
                        completed.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                        observeAcceptedSocketReleased();
#endif
                    }
                    return;
                }
                state->directCompletion = &completed.operation;
                state->owner->handleRead(observedAt);
                if (state->directCompletion == &completed.operation) {
                    state->directCompletion = nullptr;
                }
            };
        }
        detail::retainNetworkFixedStorage(
            detail::NetworkFixedStorageCategory::AcceptExFixedPool,
            slots.capacity() * sizeof(IocpAcceptSlot));
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
        observeSlotPoolCreated(depth);
#endif
    }

    ~IocpAcceptState() {
        for (auto& slot : slots) {
            if (slot.pending || slot.completionPending) {
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                observeAcceptCompleted();
#endif
                slot.pending = false;
                slot.completionPending = false;
            }
            if (sockets::isValid(slot.accepted)) {
                sockets::close(slot.accepted);
                slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                observeAcceptedSocketReleased();
#endif
            }
        }
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
        observeSlotPoolDestroyed(slots.size());
#endif
        detail::releaseNetworkFixedStorage(
            detail::NetworkFixedStorageCategory::AcceptExFixedPool,
            slots.capacity() * sizeof(IocpAcceptSlot));
    }

    std::vector<IocpAcceptSlot> slots;
    LPFN_ACCEPTEX acceptEx{nullptr};
    LPFN_GETACCEPTEXSOCKADDRS getAcceptExSockaddrs{nullptr};
    Acceptor* owner{nullptr};
    IocpOperation* directCompletion{nullptr};
    Phase phase{Phase::Accepting};
    bool retryDelayElapsed{false};
};

#endif

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort)
    : loop_(loop),
      acceptSocket_(createAcceptSocket(listenAddr.family())),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reusePort);
    // For IPv6 sockets, default to dual-stack (IPV6_V6ONLY=0) so the same
    // socket can accept both IPv4-mapped and native IPv6 connections.
    if (listenAddr.isIpv6()) {
        acceptSocket_.setIpv6Only(false);
    }
    acceptSocket_.bindAddress(listenAddr);
    sockaddr_storage localStorage{};
    if (!sockets::tryGetLocalAddr(acceptSocket_.fd(), &localStorage)) {
        throwSocketError("getsockname", sockets::lastError());
    }
    listenAddr_ = InetAddress(localStorage);
    acceptChannel_.setReadCallback([this](gamenet::base::Timestamp receiveTime) { handleRead(receiveTime); });
}

Acceptor::~Acceptor() {
    if (!loop_->isInLoopThread()) {
        LOG_FATAL << "Acceptor destroyed from non-owner thread";
    }
    if (retryTimer_.valid()) {
        loop_->cancel(retryTimer_);
        retryTimer_ = {};
    }
#ifdef _WIN32
    closePendingAccept();
#endif
    if (listening_) {
        acceptChannel_.disableAll();
        acceptChannel_.remove();
    }
}

void Acceptor::setNewConnectionCallback(NewConnectionCallback cb) {
    loop_->assertInLoopThread();
    if (listening_) {
        throw std::logic_error(
            "Acceptor callback must be configured while stopped");
    }
    newConnectionCallback_ = std::move(cb);
}

void Acceptor::setErrorCallback(AcceptorErrorCallback cb) {
    loop_->assertInLoopThread();
    if (listening_) {
        throw std::logic_error(
            "Acceptor error callback must be configured while stopped");
    }
    errorCallback_ = std::move(cb);
}

void Acceptor::setIocpAcceptDepth(std::size_t depth) {
    loop_->assertInLoopThread();
    if (depth == 0 || depth > kMaxIocpAcceptDepth) {
        throw std::invalid_argument(
            "Acceptor IOCP accept depth must be in [1, 64]");
    }
    if (listening_) {
        throw std::logic_error(
            "Acceptor IOCP accept depth must be configured before listen");
    }
    iocpAcceptDepth_ = depth;
}

std::size_t Acceptor::iocpAcceptDepth() const noexcept {
    return iocpAcceptDepth_;
}

bool Acceptor::listening() const noexcept {
    return listening_;
}

const InetAddress& Acceptor::listenAddress() const noexcept {
    return listenAddr_;
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    acceptSocket_.listen();
#ifdef _WIN32
    if (!iocpAccept_) {
        iocpAccept_ = std::make_shared<IocpAcceptState>(iocpAcceptDepth_);
        iocpAccept_->acceptEx = platform::loadAcceptEx(acceptSocket_.fd());
        if (iocpAccept_->acceptEx == nullptr) {
            throwSocketError("load AcceptEx", sockets::lastError());
        }
        iocpAccept_->getAcceptExSockaddrs = platform::loadGetAcceptExSockaddrs(acceptSocket_.fd());
        if (iocpAccept_->getAcceptExSockaddrs == nullptr) {
            throwSocketError("load GetAcceptExSockaddrs", sockets::lastError());
        }
        for (auto& slot : iocpAccept_->slots) {
            slot.operation.channel = &acceptChannel_;
        }
    }
    iocpAccept_->owner = this;
    iocpAccept_->phase = IocpAcceptState::Phase::Accepting;
#endif
    listening_ = true;
    acceptChannel_.enableReading();
#ifdef _WIN32
    fillAcceptPool();
#endif
}

void Acceptor::stop() {
    loop_->assertInLoopThread();
    if (!listening_) {
        return;
    }
    listening_ = false;
    if (retryTimer_.valid()) {
        loop_->cancel(retryTimer_);
        retryTimer_ = {};
    }
#ifdef _WIN32
    closePendingAccept();
#endif
    acceptChannel_.disableAll();
    acceptChannel_.remove();
    // Close the listen socket so the kernel rejects further connections.
    // Release the fd from the Socket RAII wrapper first to avoid double-close.
    const SocketFd fd = acceptSocket_.releaseFd();
    if (sockets::isValid(fd)) {
        sockets::close(fd);
    }
}

void Acceptor::handleRead(gamenet::base::Timestamp receiveTime) {
    (void)receiveTime;
    loop_->assertInLoopThread();

#ifdef _WIN32
    const auto state = iocpAccept_;
    if (!state) {
        return;
    }

    while (true) {
        IocpOperation* completedOperation = state->directCompletion;
        if (completedOperation != nullptr) {
            state->directCompletion = nullptr;
        } else {
            completedOperation =
                acceptChannel_.takeIocpAcceptCompletionOperation();
        }
        if (completedOperation == nullptr) {
            break;
        }
        const auto completedSlot = std::find_if(
            state->slots.begin(),
            state->slots.end(),
            [completedOperation](const IocpAcceptSlot& slot) {
                return &slot.operation == completedOperation;
            });
        if (completedSlot == state->slots.end()) {
            LOG_ERROR << "Acceptor received an unknown AcceptEx completion";
            continue;
        }
        if (!completedSlot->completionPending) {
            continue;
        }

        IocpAcceptSlot& slot = *completedSlot;
        slot.completionPending = false;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
        observeAcceptCompleted();
#endif

        const bool wasCancelling = slot.cancelling;
        slot.cancelling = false;
        if (wasCancelling ||
            state->phase == IocpAcceptState::Phase::Retrying) {
            if (sockets::isValid(slot.accepted)) {
                sockets::close(slot.accepted);
                slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                observeAcceptedSocketReleased();
#endif
            }
            maybeResumeAccept();
            continue;
        }

        if (state->phase != IocpAcceptState::Phase::Accepting ||
            !listening_) {
            if (sockets::isValid(slot.accepted)) {
                sockets::close(slot.accepted);
                slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                observeAcceptedSocketReleased();
#endif
            }
            continue;
        }

        if (slot.operation.error != 0) {
            const int error = static_cast<int>(slot.operation.error);
            if (sockets::isValid(slot.accepted)) {
                sockets::close(slot.accepted);
                slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                observeAcceptedSocketReleased();
#endif
            }
            handleAcceptError(AcceptorErrorStage::Accept, error);
            continue;
        }

        if (!platform::updateAcceptContext(slot.accepted, acceptSocket_.fd())) {
            const int error = sockets::lastError();
            sockets::close(slot.accepted);
            slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            observeAcceptedSocketReleased();
#endif
            handleAcceptError(AcceptorErrorStage::AcceptedSocketSetup, error);
            continue;
        }
        sockaddr_storage peerStorage{};
        if (!sockets::tryGetPeerAddr(slot.accepted, &peerStorage)) {
            const int error = sockets::lastError();
            sockets::close(slot.accepted);
            slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            observeAcceptedSocketReleased();
#endif
            handleAcceptError(AcceptorErrorStage::AcceptedSocketSetup, error);
            continue;
        }
        InetAddress peerAddr(peerStorage);
        const SocketFd connfd = slot.accepted;
        slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
        observeAcceptedSocketReleased();
#endif

        // Replenish the completed slot before entering the upper callback. The
        // old completion has already been published and its socket moved out,
        // so callback stop() re-entry can only cancel the new generation.
        if (listening_ &&
            state->phase == IocpAcceptState::Phase::Accepting) {
            (void)postAccept(slot);
        }

        if (newConnectionCallback_) {
            newConnectionCallback_(connfd, peerAddr);
        } else {
            sockets::close(connfd);
        }
    }

    return;
#else
    while (true) {
        InetAddress peerAddr;
        const SocketFd connfd = acceptSocket_.accept(&peerAddr);
        if (sockets::isValid(connfd)) {
            if (newConnectionCallback_) {
                newConnectionCallback_(connfd, peerAddr);
            } else {
                sockets::close(connfd);
            }
            continue;
        }

        const int error = sockets::lastError();
        if (sockets::isWouldBlock(error)) {
            break;
        }
        if (sockets::isInterrupted(error)) {
            continue;
        }
        handleAcceptError(AcceptorErrorStage::Accept, error);
        break;
    }
#endif
}

void Acceptor::handleAcceptError(AcceptorErrorStage stage, int error) {
    loop_->assertInLoopThread();
    if (!listening_) {
        return;
    }
    LOG_WARN << "Acceptor runtime error: " << error << " " << sockets::errorMessage(error);
    AcceptorErrorAction action = AcceptorErrorAction::Retry;
    if (errorCallback_) {
        try {
            action = errorCallback_(AcceptorError{.stage = stage, .errorCode = error});
        } catch (const std::exception& callbackError) {
            LOG_ERROR << "Acceptor error callback threw: " << callbackError.what();
            action = AcceptorErrorAction::Stop;
        } catch (...) {
            LOG_ERROR << "Acceptor error callback threw a non-standard exception";
            action = AcceptorErrorAction::Stop;
        }
    }
    if (action == AcceptorErrorAction::Stop) {
        stop();
        return;
    }
#ifdef _WIN32
    beginAcceptRetry();
#else
    scheduleAcceptRetry();
#endif
}

void Acceptor::scheduleAcceptRetry() {
    loop_->assertInLoopThread();
    if (!listening_ || retryTimer_.valid()) {
        return;
    }
    if (acceptChannel_.isReading()) {
        acceptChannel_.disableReading();
    }
    retryTimer_ = loop_->runAfter(std::chrono::milliseconds(100), [this] { resumeAccept(); });
}

void Acceptor::resumeAccept() {
    loop_->assertInLoopThread();
    retryTimer_ = {};
    if (!listening_) {
        return;
    }
    if (!acceptChannel_.isReading()) {
        acceptChannel_.enableReading();
    }
#ifdef _WIN32
    if (!iocpAccept_) {
        return;
    }
    iocpAccept_->retryDelayElapsed = true;
    maybeResumeAccept();
#endif
}

#ifdef _WIN32

void Acceptor::fillAcceptPool() {
    loop_->assertInLoopThread();
    const auto state = iocpAccept_;
    if (!listening_ || !state ||
        state->phase != IocpAcceptState::Phase::Accepting) {
        return;
    }

    for (auto& slot : state->slots) {
        if (slot.pending) {
            continue;
        }
        if (!postAccept(slot) ||
            iocpAccept_ != state ||
            state->phase != IocpAcceptState::Phase::Accepting) {
            break;
        }
    }
}

bool Acceptor::postAccept(IocpAcceptSlot& slot) {
    loop_->assertInLoopThread();
    const auto state = iocpAccept_;
    if (!listening_ || !state ||
        state->phase != IocpAcceptState::Phase::Accepting ||
        slot.pending || slot.completionPending) {
        return false;
    }

    if (sockets::isValid(slot.accepted)) {
        sockets::close(slot.accepted);
        slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
        observeAcceptedSocketReleased();
#endif
    }
    slot.accepted = platform::createOverlappedTcp(listenAddr_.family());
    if (!sockets::isValid(slot.accepted)) {
        handleAcceptError(
            AcceptorErrorStage::AcceptedSocketCreate,
            sockets::lastError());
        return false;
    }
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeAcceptedSocketCreated();
#endif

    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    {
        std::uint64_t observed =
            maxIocpAcceptGeneration.load(std::memory_order_relaxed);
        while (observed < slot.generation &&
               !maxIocpAcceptGeneration.compare_exchange_weak(
                   observed,
                   slot.generation,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }
#endif
    slot.operation.overlapped = OVERLAPPED{};
    slot.operation.kind = IocpOperationKind::Accept;
    slot.operation.channel = &acceptChannel_;
    slot.operation.bytesTransferred = 0;
    slot.operation.error = 0;
    slot.operation.completionObserved = false;
    slot.operation.nextPublishedCompletion = nullptr;
    slot.completionPending = false;
    slot.cancelling = false;
    if (!detail::prepareIocpOperationSubmission(slot.operation)) {
        LOG_FATAL << "AcceptEx operation generation conflict";
    }

    DWORD bytes = 0;
    BOOL ok = FALSE;
    int acceptError = 0;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    const bool injectedFailure = takeInjectedAcceptError(&acceptError);
#else
    constexpr bool injectedFailure = false;
#endif
    if (!injectedFailure) {
        ok = state->acceptEx(
            acceptSocket_.fd(),
            slot.accepted,
            slot.addresses.data(),
            0,
            kAcceptAddressBytes,
            kAcceptAddressBytes,
            &bytes,
            &slot.operation.overlapped);
        acceptError = ok ? 0 : sockets::lastError();
    }
    if (!ok && acceptError != ERROR_IO_PENDING) {
        (void)detail::rejectIocpOperationSubmission(slot.operation);
        sockets::close(slot.accepted);
        slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
        observeAcceptedSocketReleased();
#endif
        handleAcceptError(AcceptorErrorStage::Accept, acceptError);
        return false;
    }

    slot.pending = true;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeAcceptSubmitted();
#endif
    loop_->retainCompletionOperation(&slot.operation, state);
    return true;
}

void Acceptor::beginAcceptRetry() {
    loop_->assertInLoopThread();
    if (!listening_ || !iocpAccept_ ||
        iocpAccept_->phase == IocpAcceptState::Phase::Retrying) {
        return;
    }

    iocpAccept_->phase = IocpAcceptState::Phase::Retrying;
    iocpAccept_->retryDelayElapsed = false;
    cancelPendingAccepts(false);
    if (!retryTimer_.valid()) {
        retryTimer_ = loop_->runAfter(
            std::chrono::milliseconds(100),
            [this] { resumeAccept(); });
    }
}

void Acceptor::maybeResumeAccept() {
    loop_->assertInLoopThread();
    const auto state = iocpAccept_;
    if (!listening_ || !state ||
        state->phase != IocpAcceptState::Phase::Retrying ||
        !state->retryDelayElapsed) {
        return;
    }
    if (std::any_of(
            state->slots.begin(),
            state->slots.end(),
            [](const IocpAcceptSlot& slot) {
                return slot.pending || slot.completionPending;
            })) {
        return;
    }

    state->phase = IocpAcceptState::Phase::Accepting;
    state->retryDelayElapsed = false;
    fillAcceptPool();
}

void Acceptor::cancelPendingAccepts(bool shutdown) noexcept {
    const auto state = iocpAccept_;
    if (!state) {
        return;
    }

    for (auto& slot : state->slots) {
        if (shutdown) {
            slot.operation.channel = nullptr;
        }
        if (!slot.pending) {
            if (shutdown && sockets::isValid(slot.accepted)) {
                sockets::close(slot.accepted);
                slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
                observeAcceptedSocketReleased();
#endif
            }
            continue;
        }

        if (!slot.cancelling) {
            slot.cancelling = true;
            loop_->trackCompletionOperation(&slot.operation);
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            iocpAcceptCancellationCount.fetch_add(
                1,
                std::memory_order_relaxed);
#endif
            if (sockets::isValid(acceptSocket_.fd())) {
                (void)::CancelIoEx(
                    reinterpret_cast<HANDLE>(acceptSocket_.fd()),
                    &slot.operation.overlapped);
            }
        }

        if (shutdown && sockets::isValid(slot.accepted)) {
            sockets::close(slot.accepted);
            slot.accepted = kInvalidSocket;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            observeAcceptedSocketReleased();
#endif
        }
    }
}

void Acceptor::closePendingAccept() noexcept {
    if (!iocpAccept_) {
        return;
    }
    acceptChannel_.clearIocpAcceptCompletionOperations();
    iocpAccept_->phase = IocpAcceptState::Phase::Stopped;
    iocpAccept_->owner = nullptr;
    cancelPendingAccepts(true);
    iocpAccept_.reset();
}

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
namespace detail {

void resetIocpAcceptPoolObservationsForTesting() noexcept {
    currentIocpAcceptSlots.store(0, std::memory_order_release);
    peakIocpAcceptSlots.store(0, std::memory_order_release);
    currentIocpAcceptSockets.store(0, std::memory_order_release);
    peakIocpAcceptSockets.store(0, std::memory_order_release);
    currentIocpAcceptSubmissions.store(0, std::memory_order_release);
    peakIocpAcceptSubmissions.store(0, std::memory_order_release);
    iocpAcceptSubmissionCount.store(0, std::memory_order_release);
    iocpAcceptCompletionCount.store(0, std::memory_order_release);
    iocpAcceptCancellationCount.store(0, std::memory_order_release);
    maxIocpAcceptGeneration.store(0, std::memory_order_release);
    iocpAcceptPostsBeforeInjectedFailure.store(-1, std::memory_order_release);
    iocpInjectedAcceptError.store(WSAENOBUFS, std::memory_order_release);
}

IocpAcceptPoolObservations iocpAcceptPoolObservationsForTesting() noexcept {
    return IocpAcceptPoolObservations{
        .currentSlots =
            currentIocpAcceptSlots.load(std::memory_order_acquire),
        .peakSlots =
            peakIocpAcceptSlots.load(std::memory_order_acquire),
        .currentSlotSockets =
            currentIocpAcceptSockets.load(std::memory_order_acquire),
        .peakSlotSockets =
            peakIocpAcceptSockets.load(std::memory_order_acquire),
        .currentSubmitted =
            currentIocpAcceptSubmissions.load(std::memory_order_acquire),
        .peakSubmitted =
            peakIocpAcceptSubmissions.load(std::memory_order_acquire),
        .submissions =
            iocpAcceptSubmissionCount.load(std::memory_order_acquire),
        .completions =
            iocpAcceptCompletionCount.load(std::memory_order_acquire),
        .cancellationRequests =
            iocpAcceptCancellationCount.load(std::memory_order_acquire),
        .maxGeneration =
            maxIocpAcceptGeneration.load(std::memory_order_acquire),
    };
}

void injectIocpAcceptSubmissionErrorForTesting(
    std::size_t successfulSubmissionsBeforeFailure,
    int error) noexcept {
    iocpInjectedAcceptError.store(error, std::memory_order_release);
    iocpAcceptPostsBeforeInjectedFailure.store(
        static_cast<std::int64_t>(successfulSubmissionsBeforeFailure),
        std::memory_order_release);
}

}  // namespace detail
#endif

#endif

}  // namespace gamenet::net
