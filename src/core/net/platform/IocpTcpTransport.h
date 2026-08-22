#pragma once

#ifdef _WIN32

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/platform/IocpOperation.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace gamenet::net {

class Channel;

class IocpTcpTransport {
public:
    static constexpr std::size_t kReadChunkBytes = 4 * 1024;

    IocpTcpTransport(
        Channel* channel,
        void* completionContext,
        IocpCompletionConsumer readConsumer,
        IocpCompletionConsumer writeConsumer);
    ~IocpTcpTransport();

    // Returns zero once an overlapped operation owns a future completion, or
    // the synchronous WinSock error when no completion packet will be posted.
    [[nodiscard]] int startRead(std::size_t maxBytes);
    ssize_t completeRead(Buffer* input, int* savedErrno);
    bool readPending() const noexcept;
    std::size_t readStorageBytes() const noexcept;
    std::size_t peakReadStorageBytes() const noexcept;
    void releaseReadStorage() noexcept;
    void enqueueWrite(std::string payload);
    [[nodiscard]] int startWrite();
    ssize_t completeWrite(int* savedErrno);
    bool writePending() const noexcept;
    std::size_t bufferedWriteBytes() const noexcept;
    std::size_t discardBufferedWrites() noexcept;
    // Includes both kernel-pending operations and decoded terminal notices
    // whose source-private consumers have not run yet.
    bool hasPendingOperations() const noexcept;
    void cancelPendingOperations(SocketFd sockfd) noexcept;

private:
    static void observeTerminal(
        void* context,
        IocpOperationKind kind) noexcept;
    static void consumeReadCompletion(
        void* context,
        gamenet::base::Timestamp observedAt,
        bool observerCurrent);
    static void consumeWriteCompletion(
        void* context,
        gamenet::base::Timestamp observedAt,
        bool observerCurrent);

    struct WriteSegment {
        std::string bytes;
        std::size_t offset{0};
    };

    struct SharedState;
    std::shared_ptr<SharedState> sharedState_;
    Channel*& channel_;
    void*& completionContext_;
    IocpCompletionConsumer& readConsumer_;
    IocpCompletionConsumer& writeConsumer_;
    IocpOperation& readOperation_;
    IocpOperation& writeOperation_;
    std::unique_ptr<char[]>& readStorage_;
    std::size_t& readStorageBytes_;
    std::size_t& peakReadStorageBytes_;
    std::deque<WriteSegment>& writeSegments_;
    std::size_t& bufferedWriteBytes_;
    WSABUF& readBuffer_;
    WSABUF& writeBuffer_;
    ULONG& submittedWriteBytes_;
    bool& readPending_;
    bool& writePending_;
    bool& readCompletionPending_;
    bool& writeCompletionPending_;
};

namespace detail {

// Source-private deterministic fault seam. Its implementation is compiled only
// when GAMENET_BUILD_TESTING is enabled; release/benchmark builds contain no
// hook check in the submission or completion hot path.
using IocpCompletionObserverForTesting =
    void (*)(IocpOperationKind kind, int error) noexcept;

void injectNextIocpReadSubmissionErrorForTesting(int error) noexcept;
void injectNextIocpWriteSubmissionErrorForTesting(int error) noexcept;
void setIocpCompletionObserverForTesting(
    IocpCompletionObserverForTesting observer) noexcept;
std::size_t iocpReadChunkBytesForTesting() noexcept;
std::size_t iocpCurrentReadStorageBytesForTesting() noexcept;
std::size_t iocpPeakReadStorageBytesForTesting() noexcept;
std::size_t iocpMaxReadSubmissionBytesForTesting() noexcept;
std::uint64_t iocpReadStorageAllocationCountForTesting() noexcept;
std::uint64_t iocpReadStorageReleaseCountForTesting() noexcept;
std::uint64_t iocpPositiveReadCompletionCountForTesting() noexcept;
void setIocpWriteChunkLimitForTesting(std::size_t bytes) noexcept;
std::uint64_t iocpWriteSubmissionCountForTesting() noexcept;
std::size_t iocpMaxWriteSubmissionBytesForTesting() noexcept;
std::uint64_t iocpPartialWriteCompletionCountForTesting() noexcept;
std::size_t iocpPeakBufferedWriteBytesForTesting() noexcept;
std::size_t iocpPeakWriteSegmentCountForTesting() noexcept;
std::size_t iocpCurrentBufferedWriteBytesForTesting() noexcept;
std::size_t iocpCurrentWriteSegmentCountForTesting() noexcept;
void resetIocpTcpTransportFaultsForTesting() noexcept;

}  // namespace detail

}  // namespace gamenet::net

#endif
