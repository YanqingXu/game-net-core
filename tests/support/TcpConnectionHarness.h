#pragma once

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"

#include "support/SocketPair.h"

#include <memory>
#include <string>
#include <utility>

namespace gamenet::test {

inline gamenet::net::TcpConnectionPtr makeTcpConnection(
    gamenet::net::EventLoop& loop,
    ConnectedSocketPair& pair,
    std::string name) {
    const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
    const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));
    return std::make_shared<gamenet::net::TcpConnection>(
        &loop,
        std::move(name),
        pair.connectionFd,
        localAddr,
        peerAddr);
}

}  // namespace gamenet::test
