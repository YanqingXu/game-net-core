#pragma once

// Acceptor 负责监听 socket 的注册与 accept 路径，是 TCP 服务端的薄适配层。
// 它只在所属 EventLoop 线程中修改监听 Channel 和回调交付。

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketTypes.h"

#include <functional>

namespace gamenet::net {

class EventLoop;

class Acceptor : private gamenet::base::noncopyable {
public:
    using NewConnectionCallback = std::function<void(SocketFd sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort);
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback cb);
    bool listening() const noexcept;
    const InetAddress& listenAddress() const noexcept;
    void listen();
    void stop();

private:
    void handleRead(gamenet::base::Timestamp receiveTime);

    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    InetAddress listenAddr_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
};

}  // namespace gamenet::net
