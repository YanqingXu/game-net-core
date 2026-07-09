#pragma once

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"

#include "support/TestAssert.h"

namespace gamenet::test {

class TcpClientStopHarness {
public:
    explicit TcpClientStopHarness(gamenet::net::EventLoop& loop)
        : loop_(loop) {}

    void observeConnection(const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop_.isInLoopThread());
        if (conn->connected()) {
            connectedAfterStop_ = stopIssued_;
            conn->forceClose();
        }
    }

    void markStopIssued() {
        GAMENET_TEST_ASSERT(loop_.isInLoopThread());
        stopIssued_ = true;
    }

    void markServerStartedAfterStop() {
        GAMENET_TEST_ASSERT(loop_.isInLoopThread());
        serverStartedAfterStop_ = true;
    }

    void assertStopped(const gamenet::net::TcpClient& client) const {
        GAMENET_TEST_ASSERT(loop_.isInLoopThread());
        GAMENET_TEST_ASSERT(stopIssued_);
        GAMENET_TEST_ASSERT(serverStartedAfterStop_);
        GAMENET_TEST_ASSERT(!connectedAfterStop_);
        GAMENET_TEST_ASSERT(client.connection() == nullptr);
    }

private:
    gamenet::net::EventLoop& loop_;
    bool stopIssued_{false};
    bool serverStartedAfterStop_{false};
    bool connectedAfterStop_{false};
};

}  // namespace gamenet::test
