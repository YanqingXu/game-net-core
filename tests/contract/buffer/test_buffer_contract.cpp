#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/SocketTypes.h"

#include "support/TestAssert.h"
#include "support/SocketPair.h"

#include <stdexcept>
#include <string>

int main() {
    {
        gamenet::net::Buffer buffer;
        buffer.append("abcdef", 6);
        GAMENET_TEST_ASSERT(buffer.retrieveAsString(2) == "ab");

        const std::string payload(gamenet::net::Buffer::kInitialSize, 'x');
        buffer.append(payload);

        GAMENET_TEST_ASSERT(buffer.readableBytes() == 4 + payload.size());
        GAMENET_TEST_ASSERT(buffer.retrieveAsString(4) == "cdef");
        GAMENET_TEST_ASSERT(buffer.retrieveAllAsString() == payload);
        GAMENET_TEST_ASSERT(buffer.readableBytes() == 0);
        GAMENET_TEST_ASSERT(buffer.prependableBytes() == gamenet::net::Buffer::kCheapPrepend);
    }

    {
        gamenet::net::Buffer buffer;
        int savedErrno = 0;
        const ssize_t n = buffer.readFd(gamenet::net::kInvalidSocket, &savedErrno);
        GAMENET_TEST_ASSERT(n < 0);
        GAMENET_TEST_ASSERT(savedErrno != 0);
    }

    {
        gamenet::test::ConnectedSocketPair pair;
        const std::string payload(128, 'i');
        GAMENET_TEST_ASSERT(
            gamenet::net::sockets::write(pair.peerFd, payload.data(), payload.size()) ==
            static_cast<ssize_t>(payload.size()));

        gamenet::net::Buffer buffer;
        int savedErrno = 0;
        const ssize_t n = buffer.readFd(pair.connectionFd, &savedErrno, 16);
        GAMENET_TEST_ASSERT(n == 16);
        GAMENET_TEST_ASSERT(buffer.readableBytes() == 16);
        GAMENET_TEST_ASSERT(buffer.retrieveAllAsString() == payload.substr(0, 16));

        gamenet::net::sockets::close(pair.connectionFd);
        pair.connectionFd = gamenet::net::kInvalidSocket;
    }

    {
        gamenet::net::Buffer buffer({
            .maxRetainedCapacityBytes = 2048,
            .trimThresholdBytes = 128,
        });
        const auto initial = buffer.retentionSnapshot();
        GAMENET_TEST_ASSERT(
            initial.retainedCapacityBytes >=
            gamenet::net::Buffer::kCheapPrepend +
                gamenet::net::Buffer::kInitialSize);
        GAMENET_TEST_ASSERT(initial.trimCount == 0);
        GAMENET_TEST_ASSERT(!initial.trimArmed);
        GAMENET_TEST_ASSERT(
            buffer.maxRetainedCapacityBytes() == 2048);
        GAMENET_TEST_ASSERT(buffer.trimThresholdBytes() == 128);

        const std::string historicalPeak(8192, 'r');
        buffer.append(historicalPeak);
        const auto grown = buffer.retentionSnapshot();
        GAMENET_TEST_ASSERT(grown.retainedCapacityBytes > 2048);
        GAMENET_TEST_ASSERT(
            grown.peakRetainedCapacityBytes ==
            grown.retainedCapacityBytes);
        GAMENET_TEST_ASSERT(grown.trimArmed);

        buffer.retrieve(historicalPeak.size() - 256);
        GAMENET_TEST_ASSERT(buffer.readableBytes() == 256);
        GAMENET_TEST_ASSERT(!buffer.trimRetainedCapacity());
        GAMENET_TEST_ASSERT(
            buffer.retentionSnapshot().retainedCapacityBytes ==
            grown.retainedCapacityBytes);
        GAMENET_TEST_ASSERT(
            buffer.retentionSnapshot().trimCount == 0);

        buffer.retrieve(128);
        GAMENET_TEST_ASSERT(buffer.readableBytes() == 128);
        const auto trimmed = buffer.retentionSnapshot();
        GAMENET_TEST_ASSERT(
            trimmed.retainedCapacityBytes <= 2048);
        GAMENET_TEST_ASSERT(
            trimmed.peakRetainedCapacityBytes ==
            grown.peakRetainedCapacityBytes);
        GAMENET_TEST_ASSERT(trimmed.trimCount == 1);
        GAMENET_TEST_ASSERT(!trimmed.trimArmed);
        GAMENET_TEST_ASSERT(
            buffer.retrieveAllAsString() ==
            std::string(128, 'r'));

        const auto stableCapacity =
            buffer.retentionSnapshot().retainedCapacityBytes;
        for (std::size_t index = 0; index < 100; ++index) {
            buffer.append("small", 5);
            GAMENET_TEST_ASSERT(
                buffer.retrieveAllAsString() == "small");
        }
        const auto reused = buffer.retentionSnapshot();
        GAMENET_TEST_ASSERT(
            reused.retainedCapacityBytes == stableCapacity);
        GAMENET_TEST_ASSERT(reused.trimCount == 1);
    }

    {
        bool rejectedSmallTarget = false;
        try {
            gamenet::net::Buffer invalid({
                .maxRetainedCapacityBytes = 1024,
                .trimThresholdBytes = 0,
            });
            (void)invalid;
        } catch (const std::invalid_argument&) {
            rejectedSmallTarget = true;
        }
        GAMENET_TEST_ASSERT(rejectedSmallTarget);

        bool rejectedHighThreshold = false;
        try {
            gamenet::net::Buffer invalid({
                .maxRetainedCapacityBytes = 2048,
                .trimThresholdBytes = 2041,
            });
            (void)invalid;
        } catch (const std::invalid_argument&) {
            rejectedHighThreshold = true;
        }
        GAMENET_TEST_ASSERT(rejectedHighThreshold);
    }

    {
        gamenet::net::Buffer buffer;
        buffer.append(nullptr, 0);
        GAMENET_TEST_ASSERT(buffer.readableBytes() == 0);

        bool rejectedNullRange = false;
        try {
            buffer.append(nullptr, 1);
        } catch (const std::invalid_argument&) {
            rejectedNullRange = true;
        }
        GAMENET_TEST_ASSERT(rejectedNullRange);

        const char foreign = 'x';
        bool rejectedForeignEnd = false;
        try {
            buffer.retrieveUntil(&foreign);
        } catch (const std::out_of_range&) {
            rejectedForeignEnd = true;
        }
        GAMENET_TEST_ASSERT(rejectedForeignEnd);

        bool rejectedWrittenOverflow = false;
        try {
            buffer.hasWritten(buffer.writableBytes() + 1);
        } catch (const std::out_of_range&) {
            rejectedWrittenOverflow = true;
        }
        GAMENET_TEST_ASSERT(rejectedWrittenOverflow);

        bool rejectedMissingReadError = false;
        try {
            (void)buffer.readFd(gamenet::net::kInvalidSocket, nullptr);
        } catch (const std::invalid_argument&) {
            rejectedMissingReadError = true;
        }
        GAMENET_TEST_ASSERT(rejectedMissingReadError);

        bool rejectedMissingWriteError = false;
        try {
            (void)buffer.writeFd(gamenet::net::kInvalidSocket, nullptr);
        } catch (const std::invalid_argument&) {
            rejectedMissingWriteError = true;
        }
        GAMENET_TEST_ASSERT(rejectedMissingWriteError);
    }

    return 0;
}
