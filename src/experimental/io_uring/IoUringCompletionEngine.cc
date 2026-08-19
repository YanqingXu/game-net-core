#include "IoUringCompletionEngine.h"

#include "gamenet/core/net/EventLoop.h"

#include <linux/io_uring.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace gamenet::experimental::io_uring {

namespace {

constexpr std::uint64_t kCancelCqeBit = std::uint64_t{1} << 63U;
constexpr std::uint32_t kMaximumGeneration = 0x7fffffffU;
constexpr std::size_t kMaximumRingEntries = 4096;

std::system_error systemError(const char* operation, int error = errno) {
    return std::system_error(error, std::generic_category(), operation);
}

IoUringCompletionEngineOptions validatedOptions(
    gamenet::net::EventLoop* ownerLoop,
    IoUringCompletionEngineOptions options) {
    if (ownerLoop == nullptr || options.entries == 0 ||
        options.entries > kMaximumRingEntries || options.maxOperations == 0 ||
        options.maxOperations > std::numeric_limits<std::uint32_t>::max() ||
        options.maxCompletionsPerWait == 0 ||
        options.maxBytesPerOperation == 0 || options.maxOwnedBytes == 0 ||
        options.maxBytesPerOperation > options.maxOwnedBytes) {
        throw std::invalid_argument(
            "IoUringCompletionEngine requires finite positive owner and budgets");
    }
    return options;
}

std::uint64_t encodeIdentity(
    IoUringOperationIdentity identity,
    bool cancel) noexcept {
    const auto encoded =
        (static_cast<std::uint64_t>(identity.generation) << 32U) |
        (static_cast<std::uint64_t>(identity.slot) + 1U);
    return cancel ? encoded | kCancelCqeBit : encoded;
}

IoUringOperationIdentity decodeIdentity(std::uint64_t value) noexcept {
    value &= ~kCancelCqeBit;
    const auto encodedSlot = static_cast<std::uint32_t>(value);
    if (encodedSlot == 0) return {};
    return {
        .slot = encodedSlot - 1U,
        .generation = static_cast<std::uint32_t>(value >> 32U),
    };
}

bool isCancelCqe(std::uint64_t value) noexcept {
    return (value & kCancelCqeBit) != 0;
}

int boundedPollTimeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout <= std::chrono::milliseconds::zero()) return 0;
    if (timeout.count() >= INT_MAX) return INT_MAX;
    return static_cast<int>(timeout.count());
}

}  // namespace

class IoUringCompletionEngineImpl {
public:
    struct OperationSlot {
        std::uint32_t generation{};
        bool active{};
        bool noticePending{};
        bool submitted{};
        bool cancelQueued{};
        IoUringOperationKind kind{IoUringOperationKind::Receive};
        gamenet::net::SocketFd socket{gamenet::net::kInvalidSocket};
        std::vector<std::byte> storage;
        std::shared_ptr<void> lease;
        sockaddr_storage peer{};
        socklen_t peerLength{sizeof(sockaddr_storage)};
        std::size_t reservedBytes{};
    };

    struct PendingSubmission {
        std::uint64_t userData{};
        bool cancel{};
    };

    IoUringCompletionEngineImpl(
        gamenet::net::EventLoop* ownerLoop,
        IoUringCompletionEngineOptions options)
        : ownerLoop_(ownerLoop),
          options_(validatedOptions(ownerLoop, options)),
          slots_(options_.maxOperations) {
        try {
            setupRing();
            probeRequiredOperations();
        } catch (...) {
            closeRing();
            throw;
        }
    }

    ~IoUringCompletionEngineImpl() {
        try {
            if (phase_ != IoUringPhase::Shutdown) {
                (void)shutdown(std::chrono::milliseconds(250));
            }
        } catch (...) {
        }
        closeRing();
        readyNotices_.clear();
        slots_.clear();
        ownedBytes_ = 0;
    }

    IoUringCompletionEngineOptions options() const noexcept {
        return options_;
    }

    IoUringPhase phase() const noexcept {
        return phase_;
    }

    IoUringCompletionEngineMetrics metrics() const noexcept {
        return {
            .ringEntries = ringEntries_,
            .operationsAccepted = operationsAccepted_,
            .sqFullRejections = sqFullRejections_,
            .operationLimitRejections = operationLimitRejections_,
            .payloadRejections = payloadRejections_,
            .byteLimitRejections = byteLimitRejections_,
            .cancelRequests = cancelRequests_,
            .cancelCqes = cancelCqes_,
            .terminalNotices = terminalNotices_,
            .cancelledOperations = cancelledOperations_,
            .staleCqes = staleCqes_,
            .shutdownTimeouts = shutdownTimeouts_,
            .crossDomainFallbacks = 0,
            .activeOperations = activeOperations_,
            .pendingSubmissions = pendingSubmissions_.size(),
            .pendingCancelCompletions = pendingCancelCompletions_,
            .readyNotices = readyNotices_.size(),
            .ownedBytes = ownedBytes_,
        };
    }

    gamenet::net::SocketFd completionDescriptor() const noexcept {
        return ringFd_;
    }

    IoUringSubmissionOutcome enqueueAccept(
        gamenet::net::SocketFd listenSocket,
        std::shared_ptr<void> lease) {
        return enqueue(
            IoUringOperationKind::Accept,
            listenSocket,
            {},
            0,
            std::move(lease));
    }

    IoUringSubmissionOutcome enqueueRecv(
        gamenet::net::SocketFd socket,
        std::size_t maximumBytes,
        std::shared_ptr<void> lease) {
        return enqueue(
            IoUringOperationKind::Receive,
            socket,
            {},
            maximumBytes,
            std::move(lease));
    }

    IoUringSubmissionOutcome enqueueSend(
        gamenet::net::SocketFd socket,
        std::string_view payload,
        std::shared_ptr<void> lease) {
        return enqueue(
            IoUringOperationKind::Send,
            socket,
            payload,
            payload.size(),
            std::move(lease));
    }

    IoUringFlushOutcome flush() {
        assertOwnerThread();
        return flushOnce();
    }

    IoUringCancelResult cancel(IoUringOperationIdentity identity) {
        assertOwnerThread();
        if (phase_ == IoUringPhase::Shutdown) {
            return IoUringCancelResult::RejectedShutdown;
        }
        auto* slot = currentSlot(identity);
        if (slot == nullptr) return IoUringCancelResult::RejectedInvalid;
        if (!slot->submitted) {
            return IoUringCancelResult::RejectedNotSubmitted;
        }
        if (slot->cancelQueued) {
            return IoUringCancelResult::AlreadyRequested;
        }
        auto* sqe = reserveSqe();
        if (sqe == nullptr) {
            ++sqFullRejections_;
            return IoUringCancelResult::SubmissionQueueFull;
        }
        sqe->opcode = IORING_OP_ASYNC_CANCEL;
        sqe->addr = encodeIdentity(identity, false);
        sqe->cancel_flags = 0;
        sqe->user_data = encodeIdentity(identity, true);
        publishSqe(sqe);
        pendingSubmissions_.push_back({
            .userData = sqe->user_data,
            .cancel = true,
        });
        slot->cancelQueued = true;
        ++cancelRequests_;
        return IoUringCancelResult::Accepted;
    }

    IoUringWaitProgress wait(std::chrono::milliseconds timeout) {
        assertOwnerThread();
        if (phase_ == IoUringPhase::Shutdown) return {};
        flushAllOrThrow();

        IoUringWaitProgress progress;
        drainCompletionQueue(progress);
        if (progress.completionQueueEntries == 0 &&
            timeout > std::chrono::milliseconds::zero()) {
            pollfd descriptor{
                .fd = ringFd_,
                .events = POLLIN,
                .revents = 0,
            };
            int result = 0;
            do {
                result = ::poll(&descriptor, 1, boundedPollTimeout(timeout));
            } while (result < 0 && errno == EINTR);
            if (result < 0) throw systemError("poll(io_uring)");
            progress.timedOut = result == 0;
            if (result > 0) drainCompletionQueue(progress);
        }
        if (phase_ == IoUringPhase::Quiescing) driveQuiesce();
        return progress;
    }

    std::optional<IoUringCompletionNotice> takeNextNotice() {
        assertOwnerThread();
        if (readyNotices_.empty()) return std::nullopt;
        auto notice = std::move(readyNotices_.front());
        readyNotices_.pop_front();
        const auto identity = notice.identity();
        if (!identity.valid() || identity.slot >= slots_.size() ||
            slots_[identity.slot].generation != identity.generation ||
            !slots_[identity.slot].noticePending) {
            throw std::logic_error(
                "io_uring terminal notice lost its reserved operation slot");
        }
        slots_[identity.slot].noticePending = false;
        if (notice.reservedBytes_ > ownedBytes_) {
            throw std::logic_error("io_uring owned-byte accounting underflow");
        }
        ownedBytes_ -= notice.reservedBytes_;
        notice.reservedBytes_ = 0;
        return notice;
    }

    void beginQuiesce() {
        assertOwnerThread();
        if (phase_ == IoUringPhase::Shutdown) return;
        phase_ = IoUringPhase::Quiescing;
        driveQuiesce();
    }

    bool quiescent() const noexcept {
        return activeOperations_ == 0 && pendingSubmissions_.empty() &&
            pendingCancelCompletions_ == 0;
    }

    IoUringShutdownResult shutdown(std::chrono::milliseconds timeout) {
        assertOwnerThread();
        if (phase_ == IoUringPhase::Shutdown) {
            return IoUringShutdownResult::Drained;
        }
        beginQuiesce();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!quiescent()) {
            driveQuiesce();
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                ++shutdownTimeouts_;
                return IoUringShutdownResult::TimedOut;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            (void)wait(std::min(remaining, std::chrono::milliseconds(50)));
        }
        phase_ = IoUringPhase::Shutdown;
        return IoUringShutdownResult::Drained;
    }

private:
    void assertOwnerThread() const {
        ownerLoop_->assertInLoopThread();
    }

    void setupRing() {
        io_uring_params parameters{};
        const auto result = ::syscall(
            __NR_io_uring_setup,
            static_cast<unsigned>(options_.entries),
            &parameters);
        if (result < 0) throw systemError("io_uring_setup");
        ringFd_ = static_cast<int>(result);
        ringEntries_ = parameters.sq_entries;

        sqRingSize_ = parameters.sq_off.array +
            parameters.sq_entries * sizeof(unsigned);
        cqRingSize_ = parameters.cq_off.cqes +
            parameters.cq_entries * sizeof(io_uring_cqe);
        if ((parameters.features & IORING_FEAT_SINGLE_MMAP) != 0) {
            const auto mappingSize = std::max(sqRingSize_, cqRingSize_);
            sqRing_ = mapRing(IORING_OFF_SQ_RING, mappingSize);
            cqRing_ = sqRing_;
            sqRingSize_ = mappingSize;
            cqRingSize_ = mappingSize;
            singleRingMapping_ = true;
        } else {
            sqRing_ = mapRing(IORING_OFF_SQ_RING, sqRingSize_);
            cqRing_ = mapRing(IORING_OFF_CQ_RING, cqRingSize_);
        }
        sqesSize_ = parameters.sq_entries * sizeof(io_uring_sqe);
        sqesMapping_ = mapRing(IORING_OFF_SQES, sqesSize_);

        auto* sqBase = static_cast<std::byte*>(sqRing_);
        sqHead_ = reinterpret_cast<unsigned*>(sqBase + parameters.sq_off.head);
        sqTail_ = reinterpret_cast<unsigned*>(sqBase + parameters.sq_off.tail);
        sqMask_ = reinterpret_cast<unsigned*>(sqBase + parameters.sq_off.ring_mask);
        sqEntries_ = reinterpret_cast<unsigned*>(sqBase + parameters.sq_off.ring_entries);
        sqArray_ = reinterpret_cast<unsigned*>(sqBase + parameters.sq_off.array);
        sqes_ = static_cast<io_uring_sqe*>(sqesMapping_);
        cachedSqTail_ = std::atomic_ref<unsigned>(*sqTail_).load(std::memory_order_relaxed);

        auto* cqBase = static_cast<std::byte*>(cqRing_);
        cqHead_ = reinterpret_cast<unsigned*>(cqBase + parameters.cq_off.head);
        cqTail_ = reinterpret_cast<unsigned*>(cqBase + parameters.cq_off.tail);
        cqMask_ = reinterpret_cast<unsigned*>(cqBase + parameters.cq_off.ring_mask);
        cqes_ = reinterpret_cast<io_uring_cqe*>(cqBase + parameters.cq_off.cqes);
    }

    void* mapRing(std::uint64_t offset, std::size_t size) {
        void* mapping = ::mmap(
            nullptr,
            size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE,
            ringFd_,
            static_cast<off_t>(offset));
        if (mapping == MAP_FAILED) throw systemError("mmap(io_uring)");
        return mapping;
    }

    void probeRequiredOperations() {
        constexpr unsigned operationCount = 255;
        std::array<std::byte,
            sizeof(io_uring_probe) + operationCount * sizeof(io_uring_probe_op)> storage{};
        auto* probe = reinterpret_cast<io_uring_probe*>(storage.data());
        const auto result = ::syscall(
            __NR_io_uring_register,
            ringFd_,
            IORING_REGISTER_PROBE,
            probe,
            operationCount);
        if (result < 0) throw systemError("io_uring_register(PROBE)");
        for (const auto required : {
                 IORING_OP_ACCEPT,
                 IORING_OP_RECV,
                 IORING_OP_SEND,
                 IORING_OP_ASYNC_CANCEL,
             }) {
            bool supported = false;
            for (unsigned index = 0; index < probe->ops_len; ++index) {
                if (probe->ops[index].op == required &&
                    (probe->ops[index].flags & IO_URING_OP_SUPPORTED) != 0) {
                    supported = true;
                    break;
                }
            }
            if (!supported) {
                throw std::runtime_error("required one-shot io_uring opcode is unsupported");
            }
        }
    }

    void closeRing() noexcept {
        if (ringFd_ >= 0) {
            ::close(ringFd_);
            ringFd_ = -1;
        }
        if (sqesMapping_ != nullptr) {
            ::munmap(sqesMapping_, sqesSize_);
            sqesMapping_ = nullptr;
        }
        if (singleRingMapping_) {
            if (sqRing_ != nullptr) ::munmap(sqRing_, sqRingSize_);
        } else {
            if (sqRing_ != nullptr) ::munmap(sqRing_, sqRingSize_);
            if (cqRing_ != nullptr) ::munmap(cqRing_, cqRingSize_);
        }
        sqRing_ = nullptr;
        cqRing_ = nullptr;
    }

    IoUringSubmissionOutcome enqueue(
        IoUringOperationKind kind,
        gamenet::net::SocketFd socket,
        std::string_view sendPayload,
        std::size_t bytes,
        std::shared_ptr<void> lease) {
        assertOwnerThread();
        if (phase_ == IoUringPhase::Quiescing) {
            return {.result = IoUringSubmissionResult::RejectedQuiescing};
        }
        if (phase_ == IoUringPhase::Shutdown) {
            return {.result = IoUringSubmissionResult::RejectedShutdown};
        }
        if (socket == gamenet::net::kInvalidSocket ||
            (kind != IoUringOperationKind::Accept && bytes == 0)) {
            return {.result = IoUringSubmissionResult::RejectedInvalid};
        }
        if (bytes > options_.maxBytesPerOperation) {
            ++payloadRejections_;
            return {.result = IoUringSubmissionResult::PayloadTooLarge};
        }
        if (activeOperations_ + readyNotices_.size() >= options_.maxOperations) {
            ++operationLimitRejections_;
            return {.result = IoUringSubmissionResult::OperationLimit};
        }
        if (!hasSqSpace()) {
            ++sqFullRejections_;
            return {.result = IoUringSubmissionResult::SubmissionQueueFull};
        }

        std::vector<std::byte> storage;
        if (bytes != 0) storage.resize(bytes);
        if (kind == IoUringOperationKind::Send) {
            std::memcpy(storage.data(), sendPayload.data(), sendPayload.size());
        }
        const auto reservedBytes = storage.capacity();
        if (ownedBytes_ > options_.maxOwnedBytes ||
            reservedBytes > options_.maxOwnedBytes - ownedBytes_) {
            ++byteLimitRejections_;
            return {.result = IoUringSubmissionResult::ByteLimit};
        }

        const auto slotIndex = findFreeSlot();
        if (!slotIndex.has_value()) {
            ++operationLimitRejections_;
            return {.result = IoUringSubmissionResult::OperationLimit};
        }
        auto& slot = slots_[*slotIndex];
        slot.generation = slot.generation == kMaximumGeneration
            ? 1U
            : slot.generation + 1U;
        if (slot.generation == 0) slot.generation = 1;
        slot.active = true;
        slot.noticePending = false;
        slot.submitted = false;
        slot.cancelQueued = false;
        slot.kind = kind;
        slot.socket = socket;
        slot.storage = std::move(storage);
        slot.lease = std::move(lease);
        slot.peer = {};
        slot.peerLength = sizeof(sockaddr_storage);
        slot.reservedBytes = reservedBytes;
        const IoUringOperationIdentity identity{
            .slot = static_cast<std::uint32_t>(*slotIndex),
            .generation = slot.generation,
        };

        auto* sqe = reserveSqe();
        if (sqe == nullptr) {
            resetSlot(slot);
            ++sqFullRejections_;
            return {.result = IoUringSubmissionResult::SubmissionQueueFull};
        }
        sqe->fd = socket;
        sqe->user_data = encodeIdentity(identity, false);
        switch (kind) {
        case IoUringOperationKind::Accept:
            sqe->opcode = IORING_OP_ACCEPT;
            sqe->addr = reinterpret_cast<std::uint64_t>(&slot.peer);
            sqe->addr2 = reinterpret_cast<std::uint64_t>(&slot.peerLength);
            sqe->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
            break;
        case IoUringOperationKind::Receive:
            sqe->opcode = IORING_OP_RECV;
            sqe->addr = reinterpret_cast<std::uint64_t>(slot.storage.data());
            sqe->len = static_cast<std::uint32_t>(slot.storage.size());
            sqe->msg_flags = 0;
            break;
        case IoUringOperationKind::Send:
            sqe->opcode = IORING_OP_SEND;
            sqe->addr = reinterpret_cast<std::uint64_t>(slot.storage.data());
            sqe->len = static_cast<std::uint32_t>(slot.storage.size());
            sqe->msg_flags = MSG_NOSIGNAL;
            break;
        }
        publishSqe(sqe);
        pendingSubmissions_.push_back({
            .userData = sqe->user_data,
            .cancel = false,
        });
        ownedBytes_ += reservedBytes;
        ++activeOperations_;
        ++operationsAccepted_;
        return {
            .result = IoUringSubmissionResult::Accepted,
            .identity = identity,
        };
    }

    bool hasSqSpace() const noexcept {
        const auto head = std::atomic_ref<unsigned>(*sqHead_).load(
            std::memory_order_acquire);
        return cachedSqTail_ - head < *sqEntries_;
    }

    io_uring_sqe* reserveSqe() noexcept {
        if (!hasSqSpace()) return nullptr;
        const auto index = cachedSqTail_ & *sqMask_;
        auto* sqe = &sqes_[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sqArray_[index] = index;
        return sqe;
    }

    void publishSqe(io_uring_sqe*) noexcept {
        ++cachedSqTail_;
        std::atomic_ref<unsigned>(*sqTail_).store(
            cachedSqTail_, std::memory_order_release);
    }

    IoUringFlushOutcome flushOnce() {
        if (pendingSubmissions_.empty()) return {};
        const auto requested = static_cast<unsigned>(pendingSubmissions_.size());
        long result = 0;
        do {
            result = ::syscall(
                __NR_io_uring_enter,
                ringFd_,
                requested,
                0U,
                0U,
                nullptr,
                0U);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            return {
                .submitted = 0,
                .pending = pendingSubmissions_.size(),
                .nativeError = errno,
            };
        }
        const auto submitted = static_cast<std::size_t>(result);
        for (std::size_t index = 0; index < submitted; ++index) {
            const auto pending = pendingSubmissions_.front();
            pendingSubmissions_.pop_front();
            const auto identity = decodeIdentity(pending.userData);
            if (pending.cancel) {
                ++pendingCancelCompletions_;
            } else if (auto* slot = currentSlot(identity)) {
                slot->submitted = true;
            }
        }
        return {
            .submitted = submitted,
            .pending = pendingSubmissions_.size(),
            .nativeError = 0,
        };
    }

    void flushAllOrThrow() {
        while (!pendingSubmissions_.empty()) {
            const auto flushed = flushOnce();
            if (flushed.nativeError != 0) {
                throw systemError("io_uring_enter", flushed.nativeError);
            }
            if (flushed.submitted == 0) {
                throw systemError("io_uring_enter made no progress", EAGAIN);
            }
        }
    }

    void drainCompletionQueue(IoUringWaitProgress& progress) {
        auto head = std::atomic_ref<unsigned>(*cqHead_).load(std::memory_order_relaxed);
        const auto tail = std::atomic_ref<unsigned>(*cqTail_).load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(tail - head);
        const auto count = std::min(available, options_.maxCompletionsPerWait);
        for (std::size_t index = 0; index < count; ++index, ++head) {
            const auto cqe = cqes_[head & *cqMask_];
            ++progress.completionQueueEntries;
            if (isCancelCqe(cqe.user_data)) {
                if (pendingCancelCompletions_ != 0) {
                    --pendingCancelCompletions_;
                } else {
                    ++staleCqes_;
                    ++progress.staleCqes;
                }
                ++cancelCqes_;
                ++progress.internalCancelCqes;
                continue;
            }
            const auto identity = decodeIdentity(cqe.user_data);
            auto* slot = currentSlot(identity);
            if (slot == nullptr) {
                ++staleCqes_;
                ++progress.staleCqes;
                continue;
            }
            publishTerminal(identity, *slot, cqe.res);
            ++progress.terminalNotices;
        }
        std::atomic_ref<unsigned>(*cqHead_).store(head, std::memory_order_release);
        const auto remainingTail = std::atomic_ref<unsigned>(*cqTail_).load(
            std::memory_order_acquire);
        progress.budgetExhausted = remainingTail != head;
    }

    void publishTerminal(
        IoUringOperationIdentity identity,
        OperationSlot& slot,
        int result) {
        IoUringCompletionStatus status = IoUringCompletionStatus::Succeeded;
        int nativeError = 0;
        std::size_t transferred = 0;
        gamenet::net::SocketFd acceptedSocket = gamenet::net::kInvalidSocket;
        if (result < 0) {
            nativeError = -result;
            status = nativeError == ECANCELED
                ? IoUringCompletionStatus::Cancelled
                : IoUringCompletionStatus::Failed;
        } else if (slot.kind == IoUringOperationKind::Accept) {
            acceptedSocket = static_cast<gamenet::net::SocketFd>(result);
        } else {
            transferred = static_cast<std::size_t>(result);
            if (slot.kind == IoUringOperationKind::Receive) {
                slot.storage.resize(std::min(transferred, slot.storage.size()));
            }
        }
        if (status == IoUringCompletionStatus::Cancelled) {
            ++cancelledOperations_;
        }
        readyNotices_.push_back(IoUringCompletionNotice(
            identity,
            slot.kind,
            status,
            transferred,
            nativeError,
            acceptedSocket,
            std::move(slot.storage),
            std::move(slot.lease),
            slot.reservedBytes));
        slot.active = false;
        slot.noticePending = true;
        slot.submitted = false;
        slot.cancelQueued = false;
        slot.socket = gamenet::net::kInvalidSocket;
        slot.reservedBytes = 0;
        --activeOperations_;
        ++terminalNotices_;
    }

    void driveQuiesce() {
        if (phase_ != IoUringPhase::Quiescing) return;
        flushAllOrThrow();
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            auto& slot = slots_[index];
            if (!slot.active || !slot.submitted || slot.cancelQueued) continue;
            const IoUringOperationIdentity identity{
                .slot = static_cast<std::uint32_t>(index),
                .generation = slot.generation,
            };
            auto result = cancel(identity);
            if (result == IoUringCancelResult::SubmissionQueueFull) {
                flushAllOrThrow();
                result = cancel(identity);
            }
            if (result != IoUringCancelResult::Accepted &&
                result != IoUringCancelResult::AlreadyRequested) {
                throw std::runtime_error("failed to enqueue io_uring final-drain cancellation");
            }
        }
        flushAllOrThrow();
    }

    std::optional<std::size_t> findFreeSlot() const noexcept {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].active && !slots_[index].noticePending) {
                return index;
            }
        }
        return std::nullopt;
    }

    OperationSlot* currentSlot(IoUringOperationIdentity identity) noexcept {
        if (!identity.valid() || identity.slot >= slots_.size()) return nullptr;
        auto& slot = slots_[identity.slot];
        if (!slot.active || slot.generation != identity.generation) return nullptr;
        return &slot;
    }

    static void resetSlot(OperationSlot& slot) noexcept {
        slot.active = false;
        slot.noticePending = false;
        slot.submitted = false;
        slot.cancelQueued = false;
        slot.socket = gamenet::net::kInvalidSocket;
        slot.storage.clear();
        slot.lease.reset();
        slot.reservedBytes = 0;
    }

    gamenet::net::EventLoop* ownerLoop_;
    IoUringCompletionEngineOptions options_;
    std::vector<OperationSlot> slots_;
    std::deque<PendingSubmission> pendingSubmissions_;
    std::deque<IoUringCompletionNotice> readyNotices_;
    IoUringPhase phase_{IoUringPhase::Running};

    int ringFd_{-1};
    std::size_t ringEntries_{};
    void* sqRing_{};
    void* cqRing_{};
    void* sqesMapping_{};
    std::size_t sqRingSize_{};
    std::size_t cqRingSize_{};
    std::size_t sqesSize_{};
    bool singleRingMapping_{};
    unsigned* sqHead_{};
    unsigned* sqTail_{};
    unsigned* sqMask_{};
    unsigned* sqEntries_{};
    unsigned* sqArray_{};
    io_uring_sqe* sqes_{};
    unsigned cachedSqTail_{};
    unsigned* cqHead_{};
    unsigned* cqTail_{};
    unsigned* cqMask_{};
    io_uring_cqe* cqes_{};

    std::size_t activeOperations_{};
    std::size_t pendingCancelCompletions_{};
    std::size_t ownedBytes_{};
    std::uint64_t operationsAccepted_{};
    std::uint64_t sqFullRejections_{};
    std::uint64_t operationLimitRejections_{};
    std::uint64_t payloadRejections_{};
    std::uint64_t byteLimitRejections_{};
    std::uint64_t cancelRequests_{};
    std::uint64_t cancelCqes_{};
    std::uint64_t terminalNotices_{};
    std::uint64_t cancelledOperations_{};
    std::uint64_t staleCqes_{};
    std::uint64_t shutdownTimeouts_{};
};

IoUringCompletionNotice::IoUringCompletionNotice() noexcept = default;

IoUringCompletionNotice::IoUringCompletionNotice(
    IoUringOperationIdentity identity,
    IoUringOperationKind kind,
    IoUringCompletionStatus status,
    std::size_t bytesTransferred,
    int nativeError,
    gamenet::net::SocketFd acceptedSocket,
    std::vector<std::byte> payload,
    std::shared_ptr<void> lease,
    std::size_t reservedBytes) noexcept
    : identity_(identity),
      kind_(kind),
      status_(status),
      bytesTransferred_(bytesTransferred),
      nativeError_(nativeError),
      acceptedSocket_(acceptedSocket),
      payload_(std::move(payload)),
      lease_(std::move(lease)),
      reservedBytes_(reservedBytes) {}

IoUringCompletionNotice::~IoUringCompletionNotice() {
    if (acceptedSocket_ != gamenet::net::kInvalidSocket) {
        ::close(acceptedSocket_);
    }
}

IoUringCompletionNotice::IoUringCompletionNotice(
    IoUringCompletionNotice&& other) noexcept
    : identity_(other.identity_),
      kind_(other.kind_),
      status_(other.status_),
      bytesTransferred_(other.bytesTransferred_),
      nativeError_(other.nativeError_),
      acceptedSocket_(std::exchange(
          other.acceptedSocket_, gamenet::net::kInvalidSocket)),
      payload_(std::move(other.payload_)),
      lease_(std::move(other.lease_)),
      reservedBytes_(std::exchange(other.reservedBytes_, 0)) {}

IoUringCompletionNotice& IoUringCompletionNotice::operator=(
    IoUringCompletionNotice&& other) noexcept {
    if (this == &other) return *this;
    if (acceptedSocket_ != gamenet::net::kInvalidSocket) {
        ::close(acceptedSocket_);
    }
    identity_ = other.identity_;
    kind_ = other.kind_;
    status_ = other.status_;
    bytesTransferred_ = other.bytesTransferred_;
    nativeError_ = other.nativeError_;
    acceptedSocket_ = std::exchange(
        other.acceptedSocket_, gamenet::net::kInvalidSocket);
    payload_ = std::move(other.payload_);
    lease_ = std::move(other.lease_);
    reservedBytes_ = std::exchange(other.reservedBytes_, 0);
    return *this;
}

IoUringOperationIdentity IoUringCompletionNotice::identity() const noexcept {
    return identity_;
}

IoUringOperationKind IoUringCompletionNotice::kind() const noexcept {
    return kind_;
}

IoUringCompletionStatus IoUringCompletionNotice::status() const noexcept {
    return status_;
}

std::size_t IoUringCompletionNotice::bytesTransferred() const noexcept {
    return bytesTransferred_;
}

int IoUringCompletionNotice::nativeError() const noexcept {
    return nativeError_;
}

std::string_view IoUringCompletionNotice::payload() const noexcept {
    return {
        reinterpret_cast<const char*>(payload_.data()),
        payload_.size(),
    };
}

gamenet::net::SocketFd IoUringCompletionNotice::acceptedSocket() const noexcept {
    return acceptedSocket_;
}

gamenet::net::SocketFd IoUringCompletionNotice::releaseAcceptedSocket() noexcept {
    return std::exchange(acceptedSocket_, gamenet::net::kInvalidSocket);
}

IoUringCompletionEngine::IoUringCompletionEngine(
    gamenet::net::EventLoop* ownerLoop,
    IoUringCompletionEngineOptions options)
    : impl_(std::make_unique<IoUringCompletionEngineImpl>(ownerLoop, options)) {}

IoUringCompletionEngine::~IoUringCompletionEngine() = default;

IoUringCompletionEngineOptions IoUringCompletionEngine::options() const noexcept {
    return impl_->options();
}

IoUringPhase IoUringCompletionEngine::phase() const noexcept {
    return impl_->phase();
}

IoUringCompletionEngineMetrics IoUringCompletionEngine::metrics() const noexcept {
    return impl_->metrics();
}

gamenet::net::SocketFd
IoUringCompletionEngine::completionDescriptor() const noexcept {
    return impl_->completionDescriptor();
}

IoUringSubmissionOutcome IoUringCompletionEngine::enqueueAccept(
    gamenet::net::SocketFd listenSocket,
    std::shared_ptr<void> lease) {
    return impl_->enqueueAccept(listenSocket, std::move(lease));
}

IoUringSubmissionOutcome IoUringCompletionEngine::enqueueRecv(
    gamenet::net::SocketFd socket,
    std::size_t maximumBytes,
    std::shared_ptr<void> lease) {
    return impl_->enqueueRecv(socket, maximumBytes, std::move(lease));
}

IoUringSubmissionOutcome IoUringCompletionEngine::enqueueSend(
    gamenet::net::SocketFd socket,
    std::string_view payload,
    std::shared_ptr<void> lease) {
    return impl_->enqueueSend(socket, payload, std::move(lease));
}

IoUringFlushOutcome IoUringCompletionEngine::flush() {
    return impl_->flush();
}

IoUringCancelResult IoUringCompletionEngine::cancel(
    IoUringOperationIdentity identity) {
    return impl_->cancel(identity);
}

IoUringWaitProgress IoUringCompletionEngine::wait(
    std::chrono::milliseconds timeout) {
    return impl_->wait(timeout);
}

std::optional<IoUringCompletionNotice>
IoUringCompletionEngine::takeNextNotice() {
    return impl_->takeNextNotice();
}

void IoUringCompletionEngine::beginQuiesce() {
    impl_->beginQuiesce();
}

bool IoUringCompletionEngine::quiescent() const noexcept {
    return impl_->quiescent();
}

IoUringShutdownResult IoUringCompletionEngine::shutdown(
    std::chrono::milliseconds timeout) {
    return impl_->shutdown(timeout);
}

}  // namespace gamenet::experimental::io_uring
