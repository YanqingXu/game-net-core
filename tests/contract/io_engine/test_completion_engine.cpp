#include "../../../src/core/net/detail/CompletionPort.h"
#include "support/TestAssert.h"

#include <cstdint>
#include <memory>

#ifdef _WIN32
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/platform/IocpOperation.h"

#include "../../../src/core/net/detail/EventLoopControlRegistry.h"
#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"

#include <array>
#endif

namespace {

using gamenet::net::detail::CompletionOperationIdentity;
using gamenet::net::detail::CompletionOperationKind;
using gamenet::net::detail::CompletionTerminalStatus;

void testCompletionVocabularyIsOperationShaped() {
    CompletionOperationIdentity identity;
    GAMENET_TEST_ASSERT(!identity.valid());
    GAMENET_TEST_ASSERT(
        CompletionOperationKind::Read != CompletionOperationKind::Write);
    GAMENET_TEST_ASSERT(
        CompletionTerminalStatus::Succeeded !=
        CompletionTerminalStatus::Cancelled);
}

#ifdef _WIN32

using EngineHarness = gamenet::net::detail::EventLoopControlRegistry;
using CompletionHarness =
    gamenet::net::detail::EventLoopIocpAssociationHarness;
using gamenet::net::detail::IoEngineOperationResult;

struct TerminalObservation {
    int calls{0};
    gamenet::net::IocpOperationKind kind{
        gamenet::net::IocpOperationKind::Read};
};

void observeTerminal(
    void* context,
    gamenet::net::IocpOperationKind kind) noexcept {
    auto& observation = *static_cast<TerminalObservation*>(context);
    ++observation.calls;
    observation.kind = kind;
}

struct SocketPair {
    gamenet::net::SocketFd first{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd second{gamenet::net::kInvalidSocket};

    SocketPair() {
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        first = fds[0];
        second = fds[1];
    }

    ~SocketPair() {
        gamenet::net::sockets::close(first);
        gamenet::net::sockets::close(second);
    }
};

void testNativePacketsBecomeDistinctTerminalNotices() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    gamenet::net::Channel channel(&loop, sockets.first);
    channel.enableReading();

    std::array<gamenet::net::IocpOperation, 2> operations{};
    std::array<TerminalObservation, 2> terminalObservations{};
    operations[0].kind = gamenet::net::IocpOperationKind::Read;
    operations[0].channel = &channel;
    operations[1].kind = gamenet::net::IocpOperationKind::Write;
    operations[1].channel = &channel;

    for (std::size_t index = 0; index < operations.size(); ++index) {
        operations[index].terminalContext =
            &terminalObservations[index];
        operations[index].terminalObserver = &observeTerminal;
        GAMENET_TEST_ASSERT(
            CompletionHarness::beginSyntheticCompletionSubmission(
                &operations[index]));
        GAMENET_TEST_ASSERT(operations[index].generation != 0);
        GAMENET_TEST_ASSERT(
            CompletionHarness::postCompletion(
                loop,
                &operations[index],
                static_cast<DWORD>(index + 11)));
    }

    const auto batch = CompletionHarness::waitNativeCompletions(loop, 0);
    GAMENET_TEST_ASSERT(batch.notices.size() == 2);
    GAMENET_TEST_ASSERT(batch.progress.nativePackets == 2);
    GAMENET_TEST_ASSERT(batch.progress.deliveredNotices == 2);
    GAMENET_TEST_ASSERT(batch.progress.invalidPackets == 0);
    GAMENET_TEST_ASSERT(batch.notices[0].identity.valid());
    GAMENET_TEST_ASSERT(
        batch.notices[0].identity.operation == &operations[0]);
    GAMENET_TEST_ASSERT(
        batch.notices[1].identity.operation == &operations[1]);
    GAMENET_TEST_ASSERT(
        batch.notices[0].kind == CompletionOperationKind::Read);
    GAMENET_TEST_ASSERT(
        batch.notices[1].kind == CompletionOperationKind::Write);
    GAMENET_TEST_ASSERT(batch.notices[0].observer == &channel);
    GAMENET_TEST_ASSERT(batch.notices[1].observer == &channel);
    GAMENET_TEST_ASSERT(batch.notices[0].bytesTransferred == 11);
    GAMENET_TEST_ASSERT(batch.notices[1].bytesTransferred == 12);
    GAMENET_TEST_ASSERT(batch.notices[0].terminal());
    GAMENET_TEST_ASSERT(batch.notices[1].terminal());
    GAMENET_TEST_ASSERT(operations[0].completionObserved);
    GAMENET_TEST_ASSERT(operations[1].completionObserved);
    GAMENET_TEST_ASSERT(terminalObservations[0].calls == 1);
    GAMENET_TEST_ASSERT(terminalObservations[1].calls == 1);
    GAMENET_TEST_ASSERT(
        terminalObservations[0].kind ==
        gamenet::net::IocpOperationKind::Read);
    GAMENET_TEST_ASSERT(
        terminalObservations[1].kind ==
        gamenet::net::IocpOperationKind::Write);

    channel.disableAll();
    channel.remove();
}

void testGenerationRejectsDuplicateAndRejectedSubmissionPackets() {
    gamenet::net::EventLoop loop;
    gamenet::net::IocpOperation operation{};
    operation.kind = gamenet::net::IocpOperationKind::Read;

    GAMENET_TEST_ASSERT(
        CompletionHarness::beginSyntheticCompletionSubmission(&operation));
    const std::uint64_t firstGeneration = operation.generation;
    GAMENET_TEST_ASSERT(
        CompletionHarness::postCompletion(loop, &operation, 1));
    const auto first = CompletionHarness::waitNativeCompletions(loop, 0);
    GAMENET_TEST_ASSERT(first.notices.size() == 1);
    GAMENET_TEST_ASSERT(
        first.notices[0].identity.generation == firstGeneration);

    GAMENET_TEST_ASSERT(
        CompletionHarness::beginSyntheticCompletionSubmission(&operation));
    const std::uint64_t secondGeneration = operation.generation;
    GAMENET_TEST_ASSERT(secondGeneration != firstGeneration);
    GAMENET_TEST_ASSERT(
        CompletionHarness::postCompletion(loop, &operation, 2));
    const auto second = CompletionHarness::waitNativeCompletions(loop, 0);
    GAMENET_TEST_ASSERT(second.notices.size() == 1);
    GAMENET_TEST_ASSERT(
        second.notices[0].identity.generation == secondGeneration);

    GAMENET_TEST_ASSERT(
        CompletionHarness::postCompletion(loop, &operation, 3));
    const auto duplicate =
        CompletionHarness::waitNativeCompletions(loop, 0);
    GAMENET_TEST_ASSERT(duplicate.notices.empty());
    GAMENET_TEST_ASSERT(duplicate.progress.invalidPackets == 1);

    GAMENET_TEST_ASSERT(
        CompletionHarness::prepareRejectedCompletionSubmission(
            &operation));
    GAMENET_TEST_ASSERT(
        CompletionHarness::postCompletion(loop, &operation, 4));
    const auto rejected =
        CompletionHarness::waitNativeCompletions(loop, 0);
    GAMENET_TEST_ASSERT(rejected.notices.empty());
    GAMENET_TEST_ASSERT(rejected.progress.invalidPackets == 1);
}

void testObserverRevokeDoesNotRetireKernelLeaseEarly() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    gamenet::net::Channel channel(&loop, sockets.first);
    channel.enableReading();

    gamenet::net::IocpOperation operation{};
    operation.kind = gamenet::net::IocpOperationKind::Read;
    operation.channel = &channel;
    auto lifetime = std::make_shared<int>(7);
    std::weak_ptr<int> observedLifetime = lifetime;

    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionSubmission(
            loop,
            &operation,
            lifetime) == IoEngineOperationResult::Accepted);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionSubmission(
            loop,
            &operation,
            lifetime) == IoEngineOperationResult::RejectedConflict);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionCancellation(
            loop,
            &operation) == IoEngineOperationResult::Accepted);
    lifetime.reset();
    GAMENET_TEST_ASSERT(!observedLifetime.expired());

    channel.disableAll();
    channel.remove();
    operation.overlapped.Internal = 0xC0000120UL;
    GAMENET_TEST_ASSERT(
        CompletionHarness::postCompletion(loop, &operation, 0));
    const auto terminal =
        CompletionHarness::waitNativeCompletions(loop, 0);
    GAMENET_TEST_ASSERT(terminal.notices.size() == 1);
    GAMENET_TEST_ASSERT(terminal.notices[0].observer == nullptr);
    GAMENET_TEST_ASSERT(
        terminal.notices[0].status ==
        CompletionTerminalStatus::Cancelled);
    GAMENET_TEST_ASSERT(!observedLifetime.expired());
    CompletionHarness::retireNativeCompletions(loop);
    GAMENET_TEST_ASSERT(observedLifetime.expired());
    GAMENET_TEST_ASSERT(EngineHarness::ioEngineQuiescent(loop));
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionCancellation(
            loop,
            &operation) == IoEngineOperationResult::RejectedConflict);
}

#endif

}  // namespace

int main() {
    testCompletionVocabularyIsOperationShaped();
#ifdef _WIN32
    testNativePacketsBecomeDistinctTerminalNotices();
    testGenerationRejectsDuplicateAndRejectedSubmissionPackets();
    testObserverRevokeDoesNotRetireKernelLeaseEarly();
#endif
    return 0;
}
