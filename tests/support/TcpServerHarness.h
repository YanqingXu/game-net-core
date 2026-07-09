#pragma once

#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"

#include "support/ClientSocket.h"
#include "support/TestAssert.h"

#include <cstddef>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace gamenet::test {

inline std::vector<gamenet::net::SocketFd> connectTestClients(
    const gamenet::net::InetAddress& serverAddr,
    int clientCount) {
    GAMENET_TEST_ASSERT(clientCount >= 0);

    std::vector<gamenet::net::SocketFd> clientFds;
    clientFds.reserve(static_cast<std::size_t>(clientCount));
    for (int i = 0; i < clientCount; ++i) {
        clientFds.push_back(connectTestClient(serverAddr));
    }
    return clientFds;
}

class WorkerLoopTracker {
public:
    void recordCurrentThread() {
        std::lock_guard lock(mutex_);
        workerLoopIds_.insert(std::this_thread::get_id());
    }

    void requireAtLeast(std::size_t expectedCount) const {
        std::lock_guard lock(mutex_);
        GAMENET_TEST_ASSERT(workerLoopIds_.size() >= expectedCount);
    }

private:
    mutable std::mutex mutex_;
    std::set<std::thread::id> workerLoopIds_;
};

}  // namespace gamenet::test
