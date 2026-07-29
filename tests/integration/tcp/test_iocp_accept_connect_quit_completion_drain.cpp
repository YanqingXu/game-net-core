#include "support/TestAssert.h"

#ifdef _WIN32

#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"

#include <chrono>
#include <memory>

using gamenet::net::detail::EventLoopIocpAssociationHarness;

namespace {

void assertNoCompletionLeases(gamenet::net::EventLoop& loop) {
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) == 0);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::retainedCompletionCount(loop) == 0);
}

void testAcceptCancelDestroyAndImmediateQuit() {
    constexpr std::size_t kAcceptDepth = 7;
    gamenet::net::EventLoop loop;
    std::unique_ptr<gamenet::net::Acceptor> acceptor;

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        acceptor = std::make_unique<gamenet::net::Acceptor>(
            &loop,
            gamenet::net::InetAddress(0, true),
            true);
        acceptor->setIocpAcceptDepth(kAcceptDepth);
        acceptor->listen();

        // One retained lease per slot proves the complete fixed pool was
        // submitted successfully. None is a shutdown obligation until
        // owner-loop cancellation begins.
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::retainedCompletionCount(loop) ==
            kAcceptDepth);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) == 0);

        acceptor->stop();
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::retainedCompletionCount(loop) ==
            kAcceptDepth);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) ==
            kAcceptDepth);

        acceptor.reset();
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(!acceptor);
    assertNoCompletionLeases(loop);
    GAMENET_TEST_ASSERT(
        loop.phase() == gamenet::net::EventLoopPhase::Shutdown);
}

void testConnectCancelDestroyAndImmediateQuit() {
    gamenet::net::EventLoop loop;
    // Keep a loopback port bound but not listening. ConnectEx still accepts
    // the overlapped request, while no peer can complete a real connection.
    gamenet::net::Acceptor unstartedTarget(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    std::shared_ptr<gamenet::net::Connector> connector;
    std::weak_ptr<gamenet::net::Connector> connectorObserver;

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connector = std::make_shared<gamenet::net::Connector>(
            &loop,
            unstartedTarget.listenAddress());
        connectorObserver = connector;
        connector->start();

        // The first poll cannot consume this operation because submission
        // happens inside the current timer callback.
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::retainedCompletionCount(loop) == 1);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) == 0);

        connector->stop();
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::retainedCompletionCount(loop) == 1);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) == 1);
        connector->stop();
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) == 1);

        connector.reset();
        GAMENET_TEST_ASSERT(!connectorObserver.expired());
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(!connector);
    GAMENET_TEST_ASSERT(connectorObserver.expired());
    assertNoCompletionLeases(loop);
    GAMENET_TEST_ASSERT(
        loop.phase() == gamenet::net::EventLoopPhase::Shutdown);
}

}  // namespace

#endif

int main() {
#ifdef _WIN32
    testAcceptCancelDestroyAndImmediateQuit();
    testConnectCancelDestroyAndImmediateQuit();
#endif
    return 0;
}
