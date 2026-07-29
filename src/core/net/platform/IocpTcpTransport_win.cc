#include "IocpTcpTransport.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include <algorithm>
#include <limits>
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
#include <atomic>
#endif
#include <utility>

namespace gamenet::net {

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
namespace {

std::atomic<int> nextReadSubmissionError{0};
std::atomic<int> nextWriteSubmissionError{0};
std::atomic<detail::IocpCompletionObserverForTesting> completionObserver{
    nullptr};
std::atomic<std::size_t> writeChunkLimit{
    static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())};
std::atomic<std::uint64_t> writeSubmissionCount{0};
std::atomic<std::size_t> maxWriteSubmissionBytes{0};
std::atomic<std::uint64_t> partialWriteCompletionCount{0};
std::atomic<std::size_t> peakBufferedWriteBytes{0};
std::atomic<std::size_t> peakWriteSegmentCount{0};
std::atomic<std::size_t> currentBufferedWriteBytes{0};
std::atomic<std::size_t> currentWriteSegmentCount{0};

int consumeInjectedError(std::atomic<int>& source) noexcept {
    return source.exchange(0, std::memory_order_acq_rel);
}

void observeCompletion(IocpOperationKind kind, int error) noexcept {
    const auto observer = completionObserver.load(std::memory_order_acquire);
    if (observer != nullptr) {
        observer(kind, error);
    }
}

void injectSubmissionError(std::atomic<int>& target, int error) noexcept {
    // WSA_IO_PENDING describes a real kernel-owned operation. The source-
    // private seam must never manufacture that ownership obligation.
    if (error == 0 || error == WSA_IO_PENDING) {
        target.store(0, std::memory_order_release);
        return;
    }
    target.store(error, std::memory_order_release);
}

template <typename T>
void updatePeak(std::atomic<T>& peak, T value) noexcept {
    T observed = peak.load(std::memory_order_relaxed);
    while (value > observed &&
           !peak.compare_exchange_weak(
               observed,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void observeWriteQueue(
    std::size_t bufferedBytes,
    std::size_t segmentCount) noexcept {
    currentBufferedWriteBytes.store(
        bufferedBytes,
        std::memory_order_relaxed);
    currentWriteSegmentCount.store(
        segmentCount,
        std::memory_order_relaxed);
    updatePeak(peakBufferedWriteBytes, bufferedBytes);
    updatePeak(peakWriteSegmentCount, segmentCount);
}

void observeWriteSubmission(std::size_t bytes) noexcept {
    writeSubmissionCount.fetch_add(1, std::memory_order_relaxed);
    updatePeak(maxWriteSubmissionBytes, bytes);
}

}  // namespace
#endif

IocpTcpTransport::IocpTcpTransport(Channel* channel)
    : channel_(channel) {
    readOperation_.kind = IocpOperationKind::Read;
    readOperation_.channel = channel_;
    writeOperation_.kind = IocpOperationKind::Write;
    writeOperation_.channel = channel_;
}

int IocpTcpTransport::startRead(std::size_t maxBytes) {
    if (readPending_) {
        return 0;
    }
    if (maxBytes == 0) {
        return WSAEINVAL;
    }

    readOperation_.overlapped = OVERLAPPED{};
    readOperation_.kind = IocpOperationKind::Read;
    readOperation_.channel = channel_;
    readOperation_.bytesTransferred = 0;
    readOperation_.error = 0;

    readBuffer_.buf = readStorage_.data();
    readBuffer_.len = static_cast<ULONG>(std::min(readStorage_.size(), maxBytes));

    DWORD flags = 0;
    DWORD bytes = 0;
    readPending_ = true;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    const int injectedError =
        consumeInjectedError(nextReadSubmissionError);
    const int rc =
        injectedError != 0
        ? SOCKET_ERROR
        : ::WSARecv(
              channel_->fd(),
              &readBuffer_,
              1,
              &bytes,
              &flags,
              &readOperation_.overlapped,
              nullptr);
#else
    const int rc = ::WSARecv(
        channel_->fd(),
        &readBuffer_,
        1,
        &bytes,
        &flags,
        &readOperation_.overlapped,
        nullptr);
#endif
    if (rc == SOCKET_ERROR) {
        const int error =
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            injectedError != 0 ? injectedError : sockets::lastError();
#else
            sockets::lastError();
#endif
        if (error == WSA_IO_PENDING) {
            return 0;
        }
        readPending_ = false;
        readOperation_.error = static_cast<DWORD>(error);
        return error;
    }
    return 0;
}

ssize_t IocpTcpTransport::completeRead(Buffer* input, int* savedErrno) {
    readPending_ = false;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeCompletion(
        IocpOperationKind::Read,
        static_cast<int>(readOperation_.error));
#endif
    if (readOperation_.error != 0) {
        *savedErrno = static_cast<int>(readOperation_.error);
        return -1;
    }

    const auto n = static_cast<std::size_t>(readOperation_.bytesTransferred);
    if (n > 0) {
        input->append(readStorage_.data(), n);
    }
    return static_cast<ssize_t>(n);
}

bool IocpTcpTransport::readPending() const noexcept {
    return readPending_;
}

void IocpTcpTransport::enqueueWrite(std::string payload) {
    if (payload.empty()) {
        return;
    }
    const std::size_t bytes = payload.size();
    writeSegments_.push_back(WriteSegment{
        .bytes = std::move(payload),
    });
    bufferedWriteBytes_ += bytes;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeWriteQueue(bufferedWriteBytes_, writeSegments_.size());
#endif
}

int IocpTcpTransport::startWrite() {
    if (writePending_) {
        return 0;
    }
    if (writeSegments_.empty()) {
        return WSAEINVAL;
    }

    writeOperation_.overlapped = OVERLAPPED{};
    writeOperation_.kind = IocpOperationKind::Write;
    writeOperation_.channel = channel_;
    writeOperation_.bytesTransferred = 0;
    writeOperation_.error = 0;

    auto& front = writeSegments_.front();
    const std::size_t remaining = front.bytes.size() - front.offset;
    std::size_t submissionBytes = (std::min)(
        remaining,
        static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()));
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    submissionBytes = (std::min)(
        submissionBytes,
        writeChunkLimit.load(std::memory_order_acquire));
#endif
    writeBuffer_.buf = front.bytes.data() + front.offset;
    writeBuffer_.len = static_cast<ULONG>(submissionBytes);
    submittedWriteBytes_ = writeBuffer_.len;

    DWORD bytes = 0;
    writePending_ = true;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    const int injectedError =
        consumeInjectedError(nextWriteSubmissionError);
    const int rc =
        injectedError != 0
        ? SOCKET_ERROR
        : ::WSASend(
              channel_->fd(),
              &writeBuffer_,
              1,
              &bytes,
              0,
              &writeOperation_.overlapped,
              nullptr);
#else
    const int rc = ::WSASend(
        channel_->fd(),
        &writeBuffer_,
        1,
        &bytes,
        0,
        &writeOperation_.overlapped,
        nullptr);
#endif
    if (rc == SOCKET_ERROR) {
        const int error =
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            injectedError != 0 ? injectedError : sockets::lastError();
#else
            sockets::lastError();
#endif
        if (error == WSA_IO_PENDING) {
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            observeWriteSubmission(submittedWriteBytes_);
#endif
            return 0;
        }
        writePending_ = false;
        writeBuffer_ = WSABUF{};
        submittedWriteBytes_ = 0;
        writeOperation_.error = static_cast<DWORD>(error);
        return error;
    }
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeWriteSubmission(submittedWriteBytes_);
#endif
    return 0;
}

ssize_t IocpTcpTransport::completeWrite(int* savedErrno) {
    writePending_ = false;
    const std::size_t submittedBytes = submittedWriteBytes_;
    writeBuffer_ = WSABUF{};
    submittedWriteBytes_ = 0;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeCompletion(
        IocpOperationKind::Write,
        static_cast<int>(writeOperation_.error));
#endif
    if (writeOperation_.error != 0) {
        *savedErrno = static_cast<int>(writeOperation_.error);
        return -1;
    }

    const auto completed =
        static_cast<std::size_t>(writeOperation_.bytesTransferred);
    if (completed == 0 ||
        writeSegments_.empty() ||
        completed > submittedBytes) {
        *savedErrno = WSAECONNRESET;
        return -1;
    }

    auto& front = writeSegments_.front();
    const std::size_t remaining = front.bytes.size() - front.offset;
    if (completed > remaining || completed > bufferedWriteBytes_) {
        *savedErrno = WSAEFAULT;
        return -1;
    }

    front.offset += completed;
    bufferedWriteBytes_ -= completed;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    if (front.offset != front.bytes.size()) {
        partialWriteCompletionCount.fetch_add(
            1,
            std::memory_order_relaxed);
    }
#endif
    if (front.offset == front.bytes.size()) {
        writeSegments_.pop_front();
    }
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeWriteQueue(bufferedWriteBytes_, writeSegments_.size());
#endif
    return static_cast<ssize_t>(completed);
}

bool IocpTcpTransport::writePending() const noexcept {
    return writePending_;
}

std::size_t IocpTcpTransport::bufferedWriteBytes() const noexcept {
    return bufferedWriteBytes_;
}

std::size_t IocpTcpTransport::discardBufferedWrites() noexcept {
    if (writePending_) {
        LOG_FATAL << "IOCP write segments discarded before completion";
    }
    const std::size_t discarded = bufferedWriteBytes_;
    writeSegments_.clear();
    bufferedWriteBytes_ = 0;
    submittedWriteBytes_ = 0;
    writeBuffer_ = WSABUF{};
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeWriteQueue(0, 0);
#endif
    return discarded;
}

bool IocpTcpTransport::hasPendingOperations() const noexcept {
    return readPending_ || writePending_;
}

void IocpTcpTransport::cancelPendingOperations(SocketFd sockfd) noexcept {
    auto cancelOne =
        [this, sockfd](bool pending, IocpOperation* operation) noexcept {
        if (!pending) {
            return;
        }
        if (::CancelIoEx(
                reinterpret_cast<HANDLE>(sockfd),
                &operation->overlapped) != FALSE) {
            channel_->ownerLoop()->trackCompletionOperation(operation);
            return;
        }
        const DWORD error = ::GetLastError();
        if (error == ERROR_NOT_FOUND) {
            // The operation may already have completed in the kernel while
            // its packet is still queued for this owner loop.
            channel_->ownerLoop()->trackCompletionOperation(operation);
            return;
        }
        if (error == ERROR_INVALID_HANDLE) {
            return;
        }
    };

    cancelOne(readPending_, &readOperation_);
    cancelOne(writePending_, &writeOperation_);
}

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
namespace detail {

void injectNextIocpReadSubmissionErrorForTesting(int error) noexcept {
    injectSubmissionError(nextReadSubmissionError, error);
}

void injectNextIocpWriteSubmissionErrorForTesting(int error) noexcept {
    injectSubmissionError(nextWriteSubmissionError, error);
}

void setIocpCompletionObserverForTesting(
    IocpCompletionObserverForTesting observer) noexcept {
    completionObserver.store(observer, std::memory_order_release);
}

void setIocpWriteChunkLimitForTesting(std::size_t bytes) noexcept {
    const std::size_t nativeLimit =
        static_cast<std::size_t>((std::numeric_limits<ULONG>::max)());
    writeChunkLimit.store(
        (std::max)(std::size_t{1}, (std::min)(bytes, nativeLimit)),
        std::memory_order_release);
}

std::uint64_t iocpWriteSubmissionCountForTesting() noexcept {
    return writeSubmissionCount.load(std::memory_order_acquire);
}

std::size_t iocpMaxWriteSubmissionBytesForTesting() noexcept {
    return maxWriteSubmissionBytes.load(std::memory_order_acquire);
}

std::uint64_t iocpPartialWriteCompletionCountForTesting() noexcept {
    return partialWriteCompletionCount.load(std::memory_order_acquire);
}

std::size_t iocpPeakBufferedWriteBytesForTesting() noexcept {
    return peakBufferedWriteBytes.load(std::memory_order_acquire);
}

std::size_t iocpPeakWriteSegmentCountForTesting() noexcept {
    return peakWriteSegmentCount.load(std::memory_order_acquire);
}

std::size_t iocpCurrentBufferedWriteBytesForTesting() noexcept {
    return currentBufferedWriteBytes.load(std::memory_order_acquire);
}

std::size_t iocpCurrentWriteSegmentCountForTesting() noexcept {
    return currentWriteSegmentCount.load(std::memory_order_acquire);
}

void resetIocpTcpTransportFaultsForTesting() noexcept {
    nextReadSubmissionError.store(0, std::memory_order_release);
    nextWriteSubmissionError.store(0, std::memory_order_release);
    completionObserver.store(nullptr, std::memory_order_release);
    writeChunkLimit.store(
        static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()),
        std::memory_order_release);
    writeSubmissionCount.store(0, std::memory_order_release);
    maxWriteSubmissionBytes.store(0, std::memory_order_release);
    partialWriteCompletionCount.store(0, std::memory_order_release);
    peakBufferedWriteBytes.store(0, std::memory_order_release);
    peakWriteSegmentCount.store(0, std::memory_order_release);
    currentBufferedWriteBytes.store(0, std::memory_order_release);
    currentWriteSegmentCount.store(0, std::memory_order_release);
}

}  // namespace detail
#endif

}  // namespace gamenet::net

#endif
