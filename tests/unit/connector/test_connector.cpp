#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/ConnectorOptions.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include "support/TestAssert.h"
#include <chrono>
#include <memory>
#include <stdexcept>

using namespace std::chrono_literals;

int main() {
    {
        gamenet::net::ConnectorOptions options;
        GAMENET_TEST_ASSERT(options.enableRetry == false);
        options.validate();

        options.initRetryDelay = 0ms;
        bool threw = false;
        try {
            options.validate();
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        GAMENET_TEST_ASSERT(threw);
    }

    {
        gamenet::net::EventLoop loop;
        auto connector = std::make_shared<gamenet::net::Connector>(
            &loop,
            gamenet::net::InetAddress("127.0.0.1", 19999));

        GAMENET_TEST_ASSERT(connector->state() == gamenet::net::Connector::kDisconnected);
        GAMENET_TEST_ASSERT(connector->serverAddress().toIp() == "127.0.0.1");
        GAMENET_TEST_ASSERT(connector->serverAddress().port() == 19999);
    }

    {
        gamenet::net::EventLoop loop;
        auto connector = std::make_shared<gamenet::net::Connector>(
            &loop,
            gamenet::net::InetAddress("127.0.0.1", 19999));

        connector->setRetryDelay(100ms, 5s);
        GAMENET_TEST_ASSERT(connector->state() == gamenet::net::Connector::kDisconnected);
    }

    {
        gamenet::net::EventLoop loop;
        auto connector = std::make_shared<gamenet::net::Connector>(
            &loop,
            gamenet::net::InetAddress("127.0.0.1", 19999));

        bool called = false;
        connector->setNewConnectionCallback([&](gamenet::net::SocketFd) { called = true; });
        GAMENET_TEST_ASSERT(!called);
        GAMENET_TEST_ASSERT(connector->state() == gamenet::net::Connector::kDisconnected);
    }

    return 0;
}
