#include "gamenet/core/net/platform/Wakeup.h"

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/eventfd.h>

namespace gamenet::net::platform {

WakeupFdPair createWakeupFds() {
    const SocketFd eventfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd < 0) {
        LOG_SYSFATAL << "eventfd: " << std::strerror(errno);
    }
    return {.readFd = eventfd, .writeFd = eventfd};
}

void closeWakeupFds(WakeupFdPair fds) {
    sockets::close(fds.readFd);
    if (fds.writeFd != fds.readFd) {
        sockets::close(fds.writeFd);
    }
}

ssize_t writeWakeup(SocketFd fd) {
    const uint64_t one = 1;
    return sockets::write(fd, &one, sizeof(one));
}

bool drainWakeup(SocketFd fd) {
    uint64_t one = 0;
    const ssize_t n = sockets::read(fd, &one, sizeof(one));
    return n == static_cast<ssize_t>(sizeof(one));
}

}  // namespace gamenet::net::platform
