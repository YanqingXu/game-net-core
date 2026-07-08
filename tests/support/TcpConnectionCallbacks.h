#pragma once

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"

#include "support/TestAssert.h"

namespace gamenet::test {

struct TcpConnectionCallbackCounts {
    int connected{0};
    int disconnected{0};
    int closed{0};
};

inline void setCountingConnectionCallback(
    const gamenet::net::TcpConnectionPtr& connection,
    gamenet::net::EventLoop& loop,
    TcpConnectionCallbackCounts& callbacks) {
    connection->setConnectionCallback([&loop, &callbacks](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++callbacks.connected;
            return;
        }
        ++callbacks.disconnected;
    });
}

inline void assertSingleConnectDisconnectClose(const TcpConnectionCallbackCounts& callbacks) {
    GAMENET_TEST_ASSERT(callbacks.connected == 1);
    GAMENET_TEST_ASSERT(callbacks.disconnected == 1);
    GAMENET_TEST_ASSERT(callbacks.closed == 1);
}

}  // namespace gamenet::test
