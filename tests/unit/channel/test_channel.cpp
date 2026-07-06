#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"

#include "support/TestAssert.h"
#include <memory>

int main() {
    gamenet::net::EventLoop loop;
    constexpr gamenet::net::SocketFd fakeFd = static_cast<gamenet::net::SocketFd>(0);

    gamenet::net::Channel channel(&loop, fakeFd);

    bool readCalled = false;
    channel.setReadCallback([&](gamenet::base::Timestamp) { readCalled = true; });
    channel.setRevents(gamenet::net::Channel::kReadEvent);
    channel.handleEvent(gamenet::base::now());
    GAMENET_TEST_ASSERT(readCalled);

    readCalled = false;
    auto owner = std::make_shared<int>(42);
    channel.tie(owner);
    owner.reset();
    channel.setRevents(gamenet::net::Channel::kReadEvent);
    channel.handleEvent(gamenet::base::now());
    GAMENET_TEST_ASSERT(!readCalled);

    return 0;
}
