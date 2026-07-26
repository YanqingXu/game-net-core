#pragma once

#ifdef _WIN32

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/platform/IocpOperation.h"

#include <array>
#include <cstddef>
#include <vector>

namespace gamenet::net {

class Channel;

class IocpTcpTransport {
public:
    explicit IocpTcpTransport(Channel* channel);

    // Returns zero once an overlapped operation owns a future completion, or
    // the synchronous WinSock error when no completion packet will be posted.
    [[nodiscard]] int startRead(std::size_t maxBytes);
    ssize_t completeRead(Buffer* input, int* savedErrno);
    bool readPending() const noexcept;
    [[nodiscard]] int startWrite(const char* data, std::size_t len);
    ssize_t completeWrite(int* savedErrno);
    bool writePending() const noexcept;
    bool hasPendingOperations() const noexcept;
    void cancelPendingOperations(SocketFd sockfd) noexcept;

private:
    Channel* channel_;
    IocpOperation readOperation_;
    IocpOperation writeOperation_;
    std::array<char, 65536> readStorage_{};
    std::vector<char> writeStorage_;
    WSABUF readBuffer_{};
    WSABUF writeBuffer_{};
    bool readPending_{false};
    bool writePending_{false};
};

namespace detail {

// Source-private deterministic fault seam. Its implementation is compiled only
// when GAMENET_BUILD_TESTING is enabled; release/benchmark builds contain no
// hook check in the submission or completion hot path.
using IocpCompletionObserverForTesting =
    void (*)(IocpOperationKind kind, int error) noexcept;

void injectNextIocpReadSubmissionErrorForTesting(int error) noexcept;
void injectNextIocpWriteSubmissionErrorForTesting(int error) noexcept;
void setIocpCompletionObserverForTesting(
    IocpCompletionObserverForTesting observer) noexcept;
void resetIocpTcpTransportFaultsForTesting() noexcept;

}  // namespace detail

}  // namespace gamenet::net

#endif
