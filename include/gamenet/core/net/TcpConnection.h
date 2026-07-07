#pragma once

// TcpConnection owns the per-connection socket/channel/buffer state for one
// plain TCP connection. It is bound to exactly one EventLoop.

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"

#include <any>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace gamenet::net {

class Channel;
class EventLoop;
#ifdef _WIN32
class IocpTcpTransport;
#endif
class Socket;

class TcpConnection : public std::enable_shared_from_this<TcpConnection>, private gamenet::base::noncopyable {
public:
    enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };

    TcpConnection(
        EventLoop* loop,
        std::string name,
        SocketFd sockfd,
        const InetAddress& localAddr,
        const InetAddress& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const noexcept;
    const std::string& name() const noexcept;
    const InetAddress& localAddress() const noexcept;
    const InetAddress& peerAddress() const noexcept;
    bool connected() const noexcept;
    bool disconnected() const noexcept;

    void send(std::string_view message);
    void send(const void* data, std::size_t len);
    void shutdown();
    void forceClose();
    void setTcpNoDelay(bool on);

    // Connection context is loop-owned mutable state. Call setContext(), getContext()
    // only from this connection's owner EventLoop thread.
    void setContext(std::any context);
    const std::any& getContext() const;
    std::any& getContext();

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setCloseCallback(CloseCallback cb);

    void connectEstablished();
    void connectDestroyed();

private:
    void handleRead(gamenet::base::Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError(int savedErrno = 0);

    void sendInLoop(const char* data, std::size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();
    void queueWriteComplete();
    void maybeQueueHighWaterMark(std::size_t oldLen, std::size_t newLen);
    void setState(StateE state) noexcept;

    EventLoop* loop_;
    std::string name_;
    StateE state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
#ifdef _WIN32
    std::unique_ptr<IocpTcpTransport> iocpTransport_;
#endif
    InetAddress localAddr_;
    InetAddress peerAddr_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
    std::size_t highWaterMark_{0};
    std::any context_;
    bool channelAdded_{false};
    bool channelRemoved_{false};
};

}  // namespace gamenet::net
