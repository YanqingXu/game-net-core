#pragma once

// DispatchResult 统一描述上层跨线程投递和 endpoint 准入结果。
// 它不隐藏底层策略；每个调用点必须选择关闭、丢弃、重试或降级。

#include "gamenet/core/net/PostResult.h"

namespace gamenet {

enum class DispatchResult {
    Accepted,
    QueueFull,
    OwnerUnavailable,
    Shutdown,
    EndpointClosed,
    EndpointOverloaded,
    PolicyRejected,
};

constexpr DispatchResult dispatchResult(net::PostResult result) noexcept {
    switch (result) {
    case net::PostResult::Accepted:
        return DispatchResult::Accepted;
    case net::PostResult::QueueFull:
        return DispatchResult::QueueFull;
    case net::PostResult::OwnerUnavailable:
        return DispatchResult::OwnerUnavailable;
    case net::PostResult::Shutdown:
        return DispatchResult::Shutdown;
    }
    return DispatchResult::OwnerUnavailable;
}

}  // namespace gamenet
