#pragma once

// TcpConnectionOptions 定义连接输入/输出准入和读反压的值类型配置。
// 配置在建连前固定，运行期字节计数由 TcpConnection 独占执行并提供原子快照。

#include <cstddef>

namespace gamenet::net {

enum class TcpSendResult {
    Accepted,
    Closed,
    // The connection-local hard limit rejected the send.
    Overloaded,
    OwnerUnavailable,
    LoopOverloaded,
    ServerOverloaded,
    GlobalOverloaded,
    // The owner loop's bounded scheduling queue rejected cross-thread work.
    SchedulingQueueFull,
    // The owner loop sealed admission before the send could be scheduled.
    OwnerShutdown,
};

struct TcpConnectionBackpressureOptions {
    std::size_t lowWaterMarkBytes{2U * 1024U * 1024U};
    std::size_t highWaterMarkBytes{4U * 1024U * 1024U};
    std::size_t hardLimitBytes{16U * 1024U * 1024U};
    std::size_t maxInputBufferBytes{2U * 1024U * 1024U};

    // Requires 0 <= low < high <= hard and maxInputBufferBytes > 0; invalid
    // values throw std::invalid_argument. Configure before establishment.
    void validate() const;
};

}  // namespace gamenet::net
