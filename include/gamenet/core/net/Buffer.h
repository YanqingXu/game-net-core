#pragma once

// Buffer 是连接读写路径上的字节容器，负责维护可读/可写/可预留区域。
// 它不解析协议，也不做线程同步，默认由所属连接的 loop 线程独占修改。

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gamenet/core/net/SocketTypes.h"

namespace gamenet::net {

struct BufferRetentionOptions {
    // Includes the cheap-prepend region.
    std::size_t maxRetainedCapacityBytes{8U + 64U * 1024U};
    // Readable-byte low watermark that permits an armed trim.
    std::size_t trimThresholdBytes{16U * 1024U};

    void validate() const;
};

struct BufferRetentionSnapshot {
    std::size_t retainedCapacityBytes{};
    std::size_t peakRetainedCapacityBytes{};
    std::uint64_t trimCount{};
    bool trimArmed{};
};

class Buffer {
public:
    static constexpr std::size_t kCheapPrepend = 8;
    static constexpr std::size_t kInitialSize = 1024;

    explicit Buffer(BufferRetentionOptions retentionOptions = {});

    std::size_t readableBytes() const noexcept;
    std::size_t writableBytes() const noexcept;
    std::size_t prependableBytes() const noexcept;
    BufferRetentionSnapshot retentionSnapshot() const noexcept;
    std::size_t maxRetainedCapacityBytes() const noexcept;
    std::size_t trimThresholdBytes() const noexcept;
    // Owner-thread-only opportunistic trim. Returns true only when storage was
    // replaced; allocation failure leaves readable bytes and arm state intact.
    bool trimRetainedCapacity() noexcept;

    const char* peek() const noexcept;
    char* beginWrite() noexcept;
    const char* beginWrite() const noexcept;

    void retrieve(std::size_t len);
    void retrieveUntil(const char* end);
    void retrieveAll();
    std::string retrieveAllAsString();
    std::string retrieveAsString(std::size_t len);

    void append(const char* data, std::size_t len);
    void append(std::string_view data);
    void append(const std::string& data);

    void ensureWritableBytes(std::size_t len);
    void hasWritten(std::size_t len);

    // Reads at most maxReadBytes so connection-level admission can bound
    // memory growth without teaching Buffer about connection policy.
    ssize_t readFd(
        SocketFd fd,
        int* savedErrno,
        std::size_t maxReadBytes = std::numeric_limits<std::size_t>::max());
    ssize_t writeFd(SocketFd fd, int* savedErrno);

private:
    char* begin() noexcept;
    const char* begin() const noexcept;
    void makeSpace(std::size_t len);
    void updateRetentionState() noexcept;

    BufferRetentionOptions retentionOptions_;
    std::vector<char> buffer_;
    std::size_t readerIndex_;
    std::size_t writerIndex_;
    std::size_t peakRetainedCapacityBytes_{};
    std::uint64_t trimCount_{};
    bool trimArmed_{false};
};

}  // namespace gamenet::net
