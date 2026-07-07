#pragma once

#ifdef _WIN32

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/platform/IocpOperation.h"

#include <array>
#include <vector>

namespace gamenet::net {

class Channel;

class IocpTcpTransport {
public:
    explicit IocpTcpTransport(Channel* channel);

    void startRead();
    ssize_t completeRead(Buffer* input, int* savedErrno);
    bool readPending() const noexcept;
    void startWrite(const char* data, std::size_t len);
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

}  // namespace gamenet::net

#endif
