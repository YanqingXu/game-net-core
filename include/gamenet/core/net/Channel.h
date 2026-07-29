#pragma once

// Channel 表示一个 fd 在所属 EventLoop 中的事件订阅与回调分发实体。
// 它不拥有 EventLoop，也不默认拥有 fd，但必须遵守 remove-before-destroy。

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/SocketTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace gamenet::net {

class EventLoop;
#ifdef _WIN32
class Acceptor;
class IocpPoller;
struct IocpOperation;
#endif

class Channel : private gamenet::base::noncopyable {
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(gamenet::base::Timestamp)>;

    Channel(EventLoop* loop, SocketFd fd);
    ~Channel();

    void handleEvent(gamenet::base::Timestamp receiveTime);
    void setReadCallback(ReadEventCallback cb);
    void setWriteCallback(EventCallback cb);
    void setCloseCallback(EventCallback cb);
    void setErrorCallback(EventCallback cb);

    void tie(const std::shared_ptr<void>& object);

    SocketFd fd() const noexcept;
    uint32_t events() const noexcept;
    void setRevents(uint32_t revents) noexcept;
    bool isNoneEvent() const noexcept;
    bool isWriting() const noexcept;
    bool isReading() const noexcept;

    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();
    void remove();

    int index() const noexcept;
    void setIndex(int index) noexcept;

    EventLoop* ownerLoop() noexcept;

    static constexpr uint32_t kNoneEvent = 0;
    static constexpr uint32_t kReadEvent = 0x01;
    static constexpr uint32_t kWriteEvent = 0x02;
    static constexpr uint32_t kErrorEvent = 0x04;
    static constexpr uint32_t kCloseEvent = 0x08;

private:
    friend class EventLoop;
#ifdef _WIN32
    friend class Acceptor;
    friend class IocpPoller;
#endif

    void update();
    void advanceRegistrationGeneration() noexcept;
    void handleEventWithGuard(gamenet::base::Timestamp receiveTime);
#ifdef _WIN32
    void setIocpCompletionOperation(IocpOperation* operation) noexcept;
    IocpOperation* takeIocpCompletionOperation() noexcept;
    void appendIocpAcceptCompletionOperation(IocpOperation* operation) noexcept;
    IocpOperation* takeIocpAcceptCompletionOperation() noexcept;
    void clearIocpAcceptCompletionOperations() noexcept;
#endif

    EventLoop* loop_;
    const SocketFd fd_;
    uint32_t events_;
    uint32_t revents_;
    int index_;
    bool eventHandling_;
    bool addedToLoop_;
    std::uint64_t registrationGeneration_;
    std::uint64_t activeBatchEpoch_;
    std::size_t activeBatchIndex_;
#ifdef _WIN32
    IocpOperation* iocpCompletionOperation_;
    IocpOperation* iocpAcceptCompletionHead_;
    IocpOperation* iocpAcceptCompletionTail_;
#endif
    bool tied_;
    std::weak_ptr<void> tie_;
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

}  // namespace gamenet::net
