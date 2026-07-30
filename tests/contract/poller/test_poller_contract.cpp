#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/NetworkMemoryRetention.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#ifdef _WIN32
#include "gamenet/core/net/platform/IocpOperation.h"
#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"

#include <array>
#include <algorithm>
#include <vector>
#endif
#include <chrono>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

void assertCurrentFixedStorageTotal(
    const gamenet::net::NetworkFixedStorageRetentionSnapshot& snapshot) {
    GAMENET_TEST_ASSERT(
        snapshot.totalRetainedBytes ==
        snapshot.acceptExFixedPoolBytes +
            snapshot.iocpCompletionBatchBytes +
            snapshot.connectionLocalReadBytes);
    GAMENET_TEST_ASSERT(snapshot.sharedReadPoolBytes == 0);
    GAMENET_TEST_ASSERT(snapshot.sharedReadSlabBytes == 0);
    GAMENET_TEST_ASSERT(
        snapshot.peakTotalRetainedBytes >=
        snapshot.totalRetainedBytes);
}

void testFixedCompletionStorageAccounting() {
    const auto before =
        gamenet::net::networkFixedStorageRetentionSnapshot();
    assertCurrentFixedStorageTotal(before);
    GAMENET_TEST_ASSERT(before.iocpCompletionBatchBytes == 0);

    {
        gamenet::net::EventLoop loop;
        const auto active =
            gamenet::net::networkFixedStorageRetentionSnapshot();
        assertCurrentFixedStorageTotal(active);
        gamenet::net::NetworkFixedStorageRetentionSnapshot crossThread;
        std::thread observer([&] {
            crossThread =
                gamenet::net::networkFixedStorageRetentionSnapshot();
        });
        observer.join();
        GAMENET_TEST_ASSERT(
            crossThread.iocpCompletionBatchBytes ==
            active.iocpCompletionBatchBytes);
        assertCurrentFixedStorageTotal(crossThread);
#ifdef _WIN32
        GAMENET_TEST_ASSERT(
            active.iocpCompletionBatchEntriesPerLoop == 64);
        GAMENET_TEST_ASSERT(active.iocpCompletionBatchBytes > 0);
        GAMENET_TEST_ASSERT(
            active.peakIocpCompletionBatchBytes >=
            active.iocpCompletionBatchBytes);
#else
        GAMENET_TEST_ASSERT(
            active.iocpCompletionBatchEntriesPerLoop == 0);
        GAMENET_TEST_ASSERT(active.iocpCompletionBatchBytes == 0);
#endif
    }

    const auto after =
        gamenet::net::networkFixedStorageRetentionSnapshot();
    assertCurrentFixedStorageTotal(after);
    GAMENET_TEST_ASSERT(after.iocpCompletionBatchBytes == 0);
#ifdef _WIN32
    GAMENET_TEST_ASSERT(after.peakIocpCompletionBatchBytes > 0);
#else
    GAMENET_TEST_ASSERT(after.peakIocpCompletionBatchBytes == 0);
#endif
}

struct ReadablePair {
    gamenet::net::SocketFd readFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd writeFd{gamenet::net::kInvalidSocket};

    ReadablePair() {
#ifdef _WIN32
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        readFd = fds[0];
        writeFd = fds[1];
#else
        int fds[2];
        GAMENET_TEST_ASSERT(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        readFd = fds[0];
        writeFd = fds[1];
#endif
    }

    ~ReadablePair() {
        gamenet::net::sockets::close(readFd);
        gamenet::net::sockets::close(writeFd);
    }
};

void writePayload(gamenet::net::SocketFd fd, std::string_view payload) {
    const ssize_t written = gamenet::net::sockets::write(fd, payload.data(), payload.size());
    GAMENET_TEST_ASSERT(written == static_cast<ssize_t>(payload.size()));
}

void testReadableCompletion() {
    gamenet::net::EventLoop loop;
    ReadablePair pair;
    gamenet::net::Channel channel(&loop, pair.readFd);

    int readCount = 0;
    bool removed = false;

#ifdef _WIN32
    gamenet::net::IocpOperation readOperation{};
    readOperation.kind = gamenet::net::IocpOperationKind::Read;
    readOperation.channel = &channel;
    std::array<char, 16> readBufferStorage{};
    WSABUF readBuffer{};
#endif

    channel.setReadCallback([&](gamenet::base::Timestamp) {
        ++readCount;

#ifdef _WIN32
        GAMENET_TEST_ASSERT(readOperation.error == 0);
        GAMENET_TEST_ASSERT(readOperation.bytesTransferred == 4);
        GAMENET_TEST_ASSERT(std::string_view(readBufferStorage.data(), readOperation.bytesTransferred) == "ping");
#else
        char buffer[16] = {};
        const ssize_t n = gamenet::net::sockets::read(pair.readFd, buffer, sizeof(buffer));
        GAMENET_TEST_ASSERT(n == 4);
        GAMENET_TEST_ASSERT(std::string_view(buffer, static_cast<std::size_t>(n)) == "ping");
#endif

        channel.disableAll();
        channel.remove();
        GAMENET_TEST_ASSERT(!loop.hasChannel(&channel));
        removed = true;
        writePayload(pair.writeFd, "pong");
        loop.runAfter(20ms, [&] { loop.quit(); });
    });

    channel.enableReading();
    GAMENET_TEST_ASSERT(loop.hasChannel(&channel));

#ifdef _WIN32
    readBuffer.buf = readBufferStorage.data();
    readBuffer.len = static_cast<ULONG>(readBufferStorage.size());
    DWORD bytes = 0;
    DWORD flags = 0;
    const int rc = ::WSARecv(
        pair.readFd,
        &readBuffer,
        1,
        &bytes,
        &flags,
        &readOperation.overlapped,
        nullptr);
    GAMENET_TEST_ASSERT(rc == 0 || gamenet::net::sockets::lastError() == WSA_IO_PENDING);
#endif

    writePayload(pair.writeFd, "ping");
    loop.loop();

    GAMENET_TEST_ASSERT(readCount == 1);
    GAMENET_TEST_ASSERT(removed);
    GAMENET_TEST_ASSERT(!loop.hasChannel(&channel));
}

#ifdef _WIN32
void testBoundedIocpBatch() {
    using gamenet::net::IocpOperation;
    using gamenet::net::IocpOperationKind;
    using gamenet::net::detail::EventLoopIocpAssociationHarness;

    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxIocpCompletionsPerPoll = 4,
    });
    EventLoopIocpAssociationHarness::resetWakeupObservations();

    const std::size_t batchSize =
        EventLoopIocpAssociationHarness::
            configuredCompletionBatchSize(loop);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::completionBatchSize() == 64);
    GAMENET_TEST_ASSERT(batchSize == 4);

    std::vector<IocpOperation> operations(batchSize + 1);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        auto& operation = operations[index];
        operation.kind =
            index % 2 == 0
            ? IocpOperationKind::Read
            : IocpOperationKind::Write;
        operation.channel = nullptr;
        EventLoopIocpAssociationHarness::trackCompletion(
            loop,
            &operation);
        if (index == batchSize / 2) {
            for (std::size_t wakeup = 0; wakeup < 256; ++wakeup) {
                loop.wakeup();
            }
        }
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::postCompletion(
                loop,
                &operation,
                static_cast<DWORD>(index + 1)));
    }
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::logicalWakeupCount(loop) == 256);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted() == 1);

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 0);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::
            lastCompletionPacketsDrained(loop) == batchSize);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::
            lastCompletionBudgetExhausted(loop));
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsConsumed() == 1);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));

    const auto processedAfterFirstPoll =
        std::count_if(
            operations.begin(),
            operations.end(),
            [](const IocpOperation& operation) {
                return operation.bytesTransferred != 0;
            });
    GAMENET_TEST_ASSERT(
        processedAfterFirstPoll ==
        static_cast<std::ptrdiff_t>(batchSize - 1));

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 0);
    GAMENET_TEST_ASSERT(
        !EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));

    for (std::size_t index = 0; index < operations.size(); ++index) {
        GAMENET_TEST_ASSERT(operations[index].error == 0);
        GAMENET_TEST_ASSERT(
            operations[index].bytesTransferred ==
            static_cast<DWORD>(index + 1));
    }

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 0);
}

void testSameChannelCompletionDeferral() {
    using gamenet::net::IocpOperation;
    using gamenet::net::IocpOperationKind;
    using gamenet::net::detail::EventLoopIocpAssociationHarness;

    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxIocpCompletionsPerPoll = 2,
    });
    ReadablePair firstPair;
    ReadablePair secondPair;
    gamenet::net::Channel first(&loop, firstPair.readFd);
    gamenet::net::Channel second(&loop, secondPair.readFd);

    int firstReads = 0;
    int firstWrites = 0;
    int secondReads = 0;
    first.setReadCallback(
        [&](gamenet::base::Timestamp) {
            ++firstReads;
        });
    first.setWriteCallback(
        [&] {
            ++firstWrites;
        });
    second.setReadCallback(
        [&](gamenet::base::Timestamp) {
            ++secondReads;
        });
    first.enableReading();
    second.enableReading();

    std::array<IocpOperation, 3> operations{};
    operations[0].kind = IocpOperationKind::Read;
    operations[0].channel = &first;
    operations[1].kind = IocpOperationKind::Write;
    operations[1].channel = &first;
    operations[2].kind = IocpOperationKind::Read;
    operations[2].channel = &second;

    for (std::size_t index = 0; index < operations.size(); ++index) {
        EventLoopIocpAssociationHarness::trackCompletion(
            loop,
            &operations[index]);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::postCompletion(
                loop,
                &operations[index],
                static_cast<DWORD>(index + 1)));
    }

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 1);
    GAMENET_TEST_ASSERT(firstReads == 1);
    GAMENET_TEST_ASSERT(firstWrites == 0);
    GAMENET_TEST_ASSERT(secondReads == 0);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));
    GAMENET_TEST_ASSERT(operations[1].bytesTransferred == 2);

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 1);
    GAMENET_TEST_ASSERT(firstReads == 1);
    GAMENET_TEST_ASSERT(firstWrites == 1);
    GAMENET_TEST_ASSERT(secondReads == 0);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 1);
    GAMENET_TEST_ASSERT(firstReads == 1);
    GAMENET_TEST_ASSERT(firstWrites == 1);
    GAMENET_TEST_ASSERT(secondReads == 1);
    GAMENET_TEST_ASSERT(
        !EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));
    GAMENET_TEST_ASSERT(operations[1].bytesTransferred == 2);

    first.disableAll();
    first.remove();
    second.disableAll();
    second.remove();
}

void testIocpCompletionBudgetMetrics() {
    using gamenet::net::EventLoopMetricEvent;
    using gamenet::net::EventLoopMetricSample;
    using gamenet::net::IocpOperation;
    using gamenet::net::IocpOperationKind;
    using gamenet::net::detail::EventLoopIocpAssociationHarness;

    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxIocpCompletionsPerPoll = 2,
    });
    std::array<IocpOperation, 3> operations{};
    std::vector<EventLoopMetricSample> samples;
    loop.setEventLoopMetricCallback(
        [&](const EventLoopMetricSample& sample) {
            if (sample.event !=
                EventLoopMetricEvent::IocpCompletionPacketsDrained) {
                return;
            }
            samples.push_back(sample);
            if (!EventLoopIocpAssociationHarness::
                    hasPendingCompletionOperations(loop)) {
                loop.quit();
            }
        });

    for (std::size_t index = 0; index < operations.size(); ++index) {
        operations[index].kind = IocpOperationKind::Read;
        EventLoopIocpAssociationHarness::trackCompletion(
            loop,
            &operations[index]);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::postCompletion(
                loop,
                &operations[index],
                static_cast<DWORD>(index + 1)));
    }
    loop.runAfter(1s, [&] {
        GAMENET_TEST_ASSERT(false && "timed out draining IOCP metric packets");
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(samples.size() == 2);
    GAMENET_TEST_ASSERT(samples[0].drainedWork == 2);
    GAMENET_TEST_ASSERT(samples[0].remainingWork == 0);
    GAMENET_TEST_ASSERT(samples[0].budgetExhausted);
    GAMENET_TEST_ASSERT(samples[1].drainedWork == 1);
    GAMENET_TEST_ASSERT(samples[1].remainingWork == 0);
    GAMENET_TEST_ASSERT(!samples[1].budgetExhausted);
}

void testDeferredCompletionPreservesDequeuedError() {
    using gamenet::net::IocpOperation;
    using gamenet::net::IocpOperationKind;
    using gamenet::net::detail::EventLoopIocpAssociationHarness;

    gamenet::net::EventLoop loop;
    ReadablePair pair;
    gamenet::net::Channel channel(&loop, pair.readFd);

    int reads = 0;
    int writes = 0;
    channel.setReadCallback(
        [&](gamenet::base::Timestamp) {
            ++reads;
            channel.disableAll();
            channel.remove();
        });
    channel.setWriteCallback(
        [&] {
            ++writes;
        });
    channel.enableReading();

    std::array<IocpOperation, 2> operations{};
    operations[0].kind = IocpOperationKind::Read;
    operations[0].channel = &channel;
    operations[1].kind = IocpOperationKind::Write;
    operations[1].channel = &channel;
    operations[1].overlapped.Internal = 0xC0000120UL;

    for (std::size_t index = 0; index < operations.size(); ++index) {
        EventLoopIocpAssociationHarness::trackCompletion(
            loop,
            &operations[index]);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::postCompletion(
                loop,
                &operations[index],
                static_cast<DWORD>(index + 1)));
    }

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 1);
    GAMENET_TEST_ASSERT(reads == 1);
    GAMENET_TEST_ASSERT(writes == 0);
    GAMENET_TEST_ASSERT(operations[1].bytesTransferred == 2);
    GAMENET_TEST_ASSERT(
        operations[1].error == ERROR_OPERATION_ABORTED);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 0);
    GAMENET_TEST_ASSERT(reads == 1);
    GAMENET_TEST_ASSERT(writes == 0);
    GAMENET_TEST_ASSERT(
        operations[1].error == ERROR_OPERATION_ABORTED);
    GAMENET_TEST_ASSERT(
        !EventLoopIocpAssociationHarness::
            hasPendingCompletionOperations(loop));
}
#endif

}  // namespace

int main() {
    testFixedCompletionStorageAccounting();
    testReadableCompletion();
#ifdef _WIN32
    testBoundedIocpBatch();
    testSameChannelCompletionDeferral();
    testIocpCompletionBudgetMetrics();
    testDeferredCompletionPreservesDequeuedError();
#endif

    return 0;
}
