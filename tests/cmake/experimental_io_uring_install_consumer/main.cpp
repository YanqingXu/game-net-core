// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include <gamenet/experimental/io_uring/IoUringTcpClient.h>
#include <gamenet/experimental/io_uring/IoUringTcpServer.h>

#include <type_traits>

#ifndef GAMENET_EXPERIMENTAL_IO_URING
#error "installed experimental target must publish its explicit feature macro"
#endif

int main() {
    namespace uring = gamenet::experimental::io_uring;

    static_assert(!std::is_copy_constructible_v<uring::IoUringTcpServer>);
    static_assert(!std::is_copy_constructible_v<uring::IoUringTcpClient>);

    uring::IoUringTcpConnectionAdapterOptions connectionOptions;
    connectionOptions.validate();
    uring::IoUringTcpClientOptions clientOptions;
    clientOptions.validate();

    return connectionOptions.maxPendingCommands != 0 &&
                   clientOptions.maximumRetryDelay >=
                       clientOptions.initialRetryDelay &&
                   uring::IoUringTcpServerPhase::Configuring !=
                       uring::IoUringTcpServerPhase::Stopped &&
                   uring::IoUringTcpClientPhase::Idle !=
                       uring::IoUringTcpClientPhase::Stopped
               ? 0
               : 1;
}
