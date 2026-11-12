// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Non-installed UDP adapter seam. No runtime path in Core depends on UDP.
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Channel.h"
#include "EventLoopLifecycleRegistry.h"
#include "IocpPollerAccess.h"
#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#include "IocpOperationState.h"
#endif
#include <stdexcept>

namespace gamenet::net::detail {
class DatagramAccess final {
  public:
    static void retire(EventLoop& loop, std::unique_ptr<Channel> channel) noexcept {
        EventLoopLifecycleRegistry::retireRemovedChannel(loop, std::move(channel));
    }
#ifdef _WIN32
    // Allocate the storage lease BEFORE entering Winsock. Neither success nor
    // WSA_IO_PENDING can be followed by an allocating commit. No poll/callback
    // can interleave this owner-only prepare/native-call/commit transaction.
    static IocpPoller& backend(EventLoop& loop) {
        return dynamic_cast<IocpPoller&>(EventLoopLifecycleRegistry::backend(loop));
    }
    static void prepare(EventLoop& loop, IocpOperation& op, std::shared_ptr<void> lease) {
        IocpPollerAccess::prepareDatagramSubmission(backend(loop), op, std::move(lease));
    }
    static void commit(EventLoop& loop, IocpOperation& op, std::shared_ptr<void> lease) noexcept {
        if (!IocpPollerAccess::commitCompletionSubmission(backend(loop), &op, std::move(lease)))
            std::terminate();
    }
    static void reject(EventLoop& loop, IocpOperation& op) noexcept {
        IocpPollerAccess::rejectDatagramSubmission(backend(loop), op);
    }
    static void cancel(EventLoop& loop, IocpOperation& op, SocketFd fd) noexcept {
        (void)IocpPollerAccess::commitCompletionCancellation(backend(loop), &op);
        (void)::CancelIoEx(reinterpret_cast<HANDLE>(fd), &op.overlapped);
    }
#endif
};
} // namespace gamenet::net::detail
