#include "IocpTcpTransport.h"

#ifdef _WIN32

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/SocketsOps.h"

namespace gamenet::net {

IocpTcpTransport::IocpTcpTransport(Channel* channel)
    : channel_(channel) {
    readOperation_.kind = IocpOperationKind::Read;
    readOperation_.channel = channel_;
    writeOperation_.kind = IocpOperationKind::Write;
    writeOperation_.channel = channel_;
}

void IocpTcpTransport::startRead() {
    if (readPending_) {
        return;
    }

    readOperation_.overlapped = OVERLAPPED{};
    readOperation_.kind = IocpOperationKind::Read;
    readOperation_.channel = channel_;
    readOperation_.bytesTransferred = 0;
    readOperation_.error = 0;

    readBuffer_.buf = readStorage_.data();
    readBuffer_.len = static_cast<ULONG>(readStorage_.size());

    DWORD flags = 0;
    DWORD bytes = 0;
    readPending_ = true;
    const int rc = ::WSARecv(
        channel_->fd(),
        &readBuffer_,
        1,
        &bytes,
        &flags,
        &readOperation_.overlapped,
        nullptr);
    if (rc == SOCKET_ERROR && sockets::lastError() != WSA_IO_PENDING) {
        readPending_ = false;
        readOperation_.error = static_cast<DWORD>(sockets::lastError());
        channel_->setRevents(Channel::kErrorEvent | Channel::kCloseEvent);
    }
}

ssize_t IocpTcpTransport::completeRead(Buffer* input, int* savedErrno) {
    readPending_ = false;
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

void IocpTcpTransport::startWrite(const char* data, std::size_t len) {
    if (writePending_ || len == 0) {
        return;
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
    const int rc = ::WSASend(
        channel_->fd(),
        &writeBuffer_,
        1,
        &bytes,
        0,
        &writeOperation_.overlapped,
        nullptr);
    if (rc == SOCKET_ERROR && sockets::lastError() != WSA_IO_PENDING) {
        writePending_ = false;
        writeOperation_.error = static_cast<DWORD>(sockets::lastError());
        channel_->setRevents(Channel::kErrorEvent | Channel::kCloseEvent);
    }
}

ssize_t IocpTcpTransport::completeWrite(int* savedErrno) {
    writePending_ = false;
    if (writeOperation_.error != 0) {
        *savedErrno = static_cast<int>(writeOperation_.error);
        return -1;
    }
    return static_cast<ssize_t>(writeOperation_.bytesTransferred);
}

bool IocpTcpTransport::writePending() const noexcept {
    return writePending_;
}

}  // namespace gamenet::net

#endif
