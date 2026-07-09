#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/ConnectorOptions.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);

    gamenet::net::ConnectorOptions options;
    options.enableRetry = true;
    options.initRetryDelay = 100ms;
    options.maxRetryDelay = 100ms;
    options.connectTimeout = 50ms;

    auto connector = std::make_shared<gamenet::net::Connector>(&loop, acceptor.listenAddress(), options);

    bool retryScheduled = false;
    bool stopIssued = false;
    bool acceptorStartedAfterStop = false;
    bool connectedAfterStop = false;
    bool acceptedAfterStop = false;
    int connectAttemptCount = 0;

    acceptor.setNewConnectionCallback([&](gamenet::net::SocketFd sockfd, const gamenet::net::InetAddress&) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        acceptedAfterStop = true;
        gamenet::net::sockets::close(sockfd);
        GAMENET_TEST_ASSERT(false && "stale Connector retry reached Acceptor after stop");
    });

    connector->setNewConnectionCallback([&](gamenet::net::SocketFd sockfd) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        connectedAfterStop = true;
        gamenet::net::sockets::close(sockfd);
        GAMENET_TEST_ASSERT(false && "stale Connector retry delivered fd after stop");
    });

    connector->setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&, gamenet::net::ConnectorEvent event) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (event == gamenet::net::ConnectorEvent::ConnectAttempt) {
                ++connectAttemptCount;
            }
            if (event != gamenet::net::ConnectorEvent::RetryScheduled || retryScheduled) {
                return;
            }

            retryScheduled = true;
            loop.runAfter(20ms, [&] {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                // connector-retry-stop-cancels-pending-retry: stop() after a
                // retry timer is scheduled must prevent a later listener from
                // receiving a stale retry connection.
                stopIssued = true;
                connector->stop();
                acceptor.listen();
                acceptorStartedAfterStop = true;
            });
        });

    connector->start();

    loop.runAfter(500ms, [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        GAMENET_TEST_ASSERT(retryScheduled);
        GAMENET_TEST_ASSERT(stopIssued);
        GAMENET_TEST_ASSERT(acceptorStartedAfterStop);
        GAMENET_TEST_ASSERT(!connectedAfterStop);
        GAMENET_TEST_ASSERT(!acceptedAfterStop);
        GAMENET_TEST_ASSERT(connectAttemptCount == 1);
        GAMENET_TEST_ASSERT(connector->state() == gamenet::net::Connector::kDisconnected);
        acceptor.stop();
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(retryScheduled);
    GAMENET_TEST_ASSERT(stopIssued);
    GAMENET_TEST_ASSERT(acceptorStartedAfterStop);
    GAMENET_TEST_ASSERT(!connectedAfterStop);
    GAMENET_TEST_ASSERT(!acceptedAfterStop);
    GAMENET_TEST_ASSERT(connectAttemptCount == 1);
    GAMENET_TEST_ASSERT(connector->state() == gamenet::net::Connector::kDisconnected);

    return 0;
}
