#include "IocpTcpTransport.h"

#ifdef _WIN32

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include <algorithm>
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
#include <atomic>
#endif

namespace gamenet::net {

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
namespace {

std::atomic<int> nextReadSubmissionError{0};
std::atomic<int> nextWriteSubmissionError{0};
std::atomic<detail::IocpCompletionObserverForTesting> completionObserver{
    nullptr};

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

int IocpTcpTransport::startWrite(const char* data, std::size_t len) {
    if (writePending_) {
        return 0;
    }
    if (len == 0) {
        return WSAEINVAL;
    }

    writeStorage_.assign(data, data + len);
    writeOperation_.overlapped = OVERLAPPED{};
    writeOperation_.kind = IocpOperationKind::Write;
    writeOperation_.channel = channel_;
    writeOperation_.bytesTransferred = 0;
    writeOperation_.error = 0;

    writeBuffer_.buf = writeStorage_.data();
    writeBuffer_.len = static_cast<ULONG>(writeStorage_.size());

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
            return 0;
        }
        writePending_ = false;
        writeOperation_.error = static_cast<DWORD>(error);
        return error;
    }
    return 0;
}

ssize_t IocpTcpTransport::completeWrite(int* savedErrno) {
    writePending_ = false;
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    observeCompletion(
        IocpOperationKind::Write,
        static_cast<int>(writeOperation_.error));
#endif
    if (writeOperation_.error != 0) {
        *savedErrno = static_cast<int>(writeOperation_.error);
        return -1;
    }
    return static_cast<ssize_t>(writeOperation_.bytesTransferred);
}

bool IocpTcpTransport::writePending() const noexcept {
    return writePending_;
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

void resetIocpTcpTransportFaultsForTesting() noexcept {
    nextReadSubmissionError.store(0, std::memory_order_release);
    nextWriteSubmissionError.store(0, std::memory_order_release);
    completionObserver.store(nullptr, std::memory_order_release);
}

}  // namespace detail
#endif

}  // namespace gamenet::net

#endif
