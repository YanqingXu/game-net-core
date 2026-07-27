#pragma once

// TcpClientControl is a copyable, lifetime-independent control handle. It does
// not own TcpClient or EventLoop; calls after TcpClient destruction return
// OwnerUnavailable.

#include "gamenet/core/net/PostResult.h"

#include <memory>

namespace gamenet::net {

class TcpClient;

class TcpClientControl {
public:
    TcpClientControl() = default;

    PostResult tryConnect() const noexcept;
    PostResult tryDisconnect() const noexcept;
    PostResult tryStop() const noexcept;
    bool available() const noexcept;

private:
    struct State;

    explicit TcpClientControl(std::shared_ptr<State> state) noexcept;
    PostResult post(unsigned operation) const noexcept;

    std::shared_ptr<State> state_;

    friend class TcpClient;
};

}  // namespace gamenet::net
