#include "../../../src/core/net/detail/CompletionPort.h"
#include "support/TestAssert.h"

#include <chrono>
#include <cstddef>
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

struct DirectCompletionObservation {
    gamenet::net::EventLoop* loop{nullptr};
    gamenet::net::Channel* channel{nullptr};
    int terminalCalls{0};
    int calls{0};
    int expectedCalls{0};
    bool replaceObserverOnFirstCall{false};
    bool quitOnTerminal{false};
    gamenet::base::Timestamp lastObservedAt{};
};

void consumeDirectCompletion(
    void* context,
    gamenet::base::Timestamp observedAt,
    bool observerCurrent) {
    auto& observation =
        *static_cast<DirectCompletionObservation*>(context);
    ++observation.terminalCalls;
    if (!observerCurrent) {
        if (observation.quitOnTerminal) {
            observation.loop->quit();
        }
        return;
    }
    ++observation.calls;
    observation.lastObservedAt = observedAt;
    if (observation.replaceObserverOnFirstCall &&
        observation.calls == 1) {
        observation.channel->disableAll();
        observation.channel->remove();
        CompletionHarness::preserveSocketAssociation(
            *observation.loop,
            observation.channel->fd());
        observation.channel->enableReading();
        observation.loop->quit();
        return;
    }
    if (observation.calls == observation.expectedCalls) {
        observation.loop->quit();
    }
}

struct RecreatedObserverObservation {
    gamenet::net::EventLoop* loop{nullptr};
    gamenet::net::Channel* channel{nullptr};
    gamenet::net::SocketFd source{gamenet::net::kInvalidSocket};
    int terminalCalls{0};
    int calls{0};
    bool replaced{false};
};

void consumeCompletionAcrossSameAddressReplacement(
    void* context,
    gamenet::base::Timestamp,
    bool observerCurrent) {
    auto& observation =
        *static_cast<RecreatedObserverObservation*>(context);
    ++observation.terminalCalls;
    if (observerCurrent) {
        ++observation.calls;
        if (!observation.replaced) {
            observation.channel->disableAll();
            observation.channel->remove();
            CompletionHarness::preserveSocketAssociation(
                *observation.loop,
                observation.source);
            GAMENET_TEST_ASSERT(
                CompletionHarness::tracks(
                    *observation.loop,
                    observation.source));
            std::destroy_at(observation.channel);
            std::construct_at(
                observation.channel,
                observation.loop,
                observation.source);
            observation.channel->enableReading();
            observation.replaced = true;
        }
    }
    if (observation.terminalCalls == 2) {
        observation.loop->quit();
    }
}

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

void testEventLoopDirectlyDispatchesAllOperationKindsWithinOwnerBudget() {
    gamenet::net::EventLoopOptions options;
    options.maxActiveChannelsPerIteration = 1;
    gamenet::net::EventLoop loop(options);
    SocketPair sockets;
    gamenet::net::Channel channel(&loop, sockets.first);
    int fakeReadinessCalls = 0;
    channel.setReadCallback(
        [&fakeReadinessCalls](gamenet::base::Timestamp) {
            ++fakeReadinessCalls;
        });
    channel.setWriteCallback([&fakeReadinessCalls] {
        ++fakeReadinessCalls;
    });
    channel.enableReading();

    DirectCompletionObservation observation{
        .loop = &loop,
        .channel = &channel,
        .expectedCalls = 4,
    };
    std::array<gamenet::net::IocpOperation, 4> operations{};
    operations[0].kind = gamenet::net::IocpOperationKind::Accept;
    operations[1].kind = gamenet::net::IocpOperationKind::Connect;
    operations[2].kind = gamenet::net::IocpOperationKind::Read;
    operations[3].kind = gamenet::net::IocpOperationKind::Write;
    for (auto& operation : operations) {
        operation.channel = &channel;
        operation.completionContext = &observation;
        operation.completionConsumer = &consumeDirectCompletion;
        GAMENET_TEST_ASSERT(
            CompletionHarness::beginSyntheticCompletionSubmission(
                &operation));
        GAMENET_TEST_ASSERT(
            CompletionHarness::postCompletion(loop, &operation, 1));
    }
    loop.runAfter(std::chrono::seconds(1), [&loop] { loop.quit(); });

    loop.loop();

    GAMENET_TEST_ASSERT(observation.calls == 4);
    GAMENET_TEST_ASSERT(observation.terminalCalls == 4);
    GAMENET_TEST_ASSERT(fakeReadinessCalls == 0);
    GAMENET_TEST_ASSERT(
        observation.lastObservedAt != gamenet::base::Timestamp{});
    channel.disableAll();
    channel.remove();
}

void testDirectDispatchRevalidatesObserverAfterReentry() {
    gamenet::net::EventLoopOptions options;
    options.maxActiveChannelsPerIteration = 1;
    gamenet::net::EventLoop loop(options);
    SocketPair sockets;
    gamenet::net::Channel channel(&loop, sockets.first);
    channel.enableReading();

    DirectCompletionObservation observation{
        .loop = &loop,
        .channel = &channel,
        .expectedCalls = 2,
        .replaceObserverOnFirstCall = true,
    };
    std::array<gamenet::net::IocpOperation, 2> operations{};
    operations[0].kind = gamenet::net::IocpOperationKind::Accept;
    operations[1].kind = gamenet::net::IocpOperationKind::Connect;
    for (auto& operation : operations) {
        operation.channel = &channel;
        operation.completionContext = &observation;
        operation.completionConsumer = &consumeDirectCompletion;
        GAMENET_TEST_ASSERT(
            CompletionHarness::beginSyntheticCompletionSubmission(
                &operation));
        GAMENET_TEST_ASSERT(
            CompletionHarness::postCompletion(loop, &operation, 1));
    }
    loop.runAfter(std::chrono::seconds(1), [&loop] { loop.quit(); });

    loop.loop();

    GAMENET_TEST_ASSERT(observation.calls == 1);
    GAMENET_TEST_ASSERT(observation.terminalCalls == 2);
    channel.disableAll();
    channel.remove();
}

void testSubmissionCapturesObserverBeforeNativeDequeue() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    gamenet::net::Channel channel(&loop, sockets.first);
    channel.enableReading();

    DirectCompletionObservation observation{
        .loop = &loop,
        .channel = &channel,
        .expectedCalls = 1,
        .quitOnTerminal = true,
    };
    gamenet::net::IocpOperation operation{};
    operation.kind = gamenet::net::IocpOperationKind::Read;
    operation.channel = &channel;
    operation.completionContext = &observation;
    operation.completionConsumer = &consumeDirectCompletion;
    auto lifetime = std::make_shared<int>(7);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionSubmission(
            loop,
            &operation,
            lifetime) == IoEngineOperationResult::Accepted);
    GAMENET_TEST_ASSERT(operation.observerIdentityCaptured);
    GAMENET_TEST_ASSERT(
        operation.observerSource == sockets.first);
    GAMENET_TEST_ASSERT(
        operation.observerRegistrationGeneration != 0);

    channel.disableAll();
    channel.remove();
    CompletionHarness::preserveSocketAssociation(
        loop,
        sockets.first);
    channel.enableReading();
    GAMENET_TEST_ASSERT(
        CompletionHarness::postCompletion(loop, &operation, 1));
    loop.runAfter(std::chrono::seconds(1), [&loop] { loop.quit(); });

    loop.loop();

    GAMENET_TEST_ASSERT(observation.terminalCalls == 1);
    GAMENET_TEST_ASSERT(observation.calls == 0);
    channel.disableAll();
    channel.remove();
}

void testSameAddressObserverReplacementCannotReviveOldCompletion() {
    gamenet::net::EventLoop loop;
    SocketPair sockets;
    alignas(gamenet::net::Channel)
        std::byte channelStorage[sizeof(gamenet::net::Channel)]{};
    auto* channel = std::construct_at(
        reinterpret_cast<gamenet::net::Channel*>(channelStorage),
        &loop,
        sockets.first);
    channel->enableReading();

    RecreatedObserverObservation observation{
        .loop = &loop,
        .channel = channel,
        .source = sockets.first,
    };
    std::array<gamenet::net::IocpOperation, 2> operations{};
    std::array<std::shared_ptr<int>, 2> lifetimes{
        std::make_shared<int>(1),
        std::make_shared<int>(2),
    };
    for (std::size_t index = 0; index < operations.size(); ++index) {
        auto& operation = operations[index];
        operation.kind = index == 0
            ? gamenet::net::IocpOperationKind::Read
            : gamenet::net::IocpOperationKind::Write;
        operation.channel = channel;
        operation.completionContext = &observation;
        operation.completionConsumer =
            &consumeCompletionAcrossSameAddressReplacement;
        GAMENET_TEST_ASSERT(
            EngineHarness::commitIoEngineCompletionSubmission(
                loop,
                &operation,
                lifetimes[index]) == IoEngineOperationResult::Accepted);
        GAMENET_TEST_ASSERT(
            CompletionHarness::postCompletion(loop, &operation, 1));
    }
    loop.runAfter(std::chrono::seconds(1), [&loop] { loop.quit(); });

    loop.loop();

    GAMENET_TEST_ASSERT(observation.replaced);
    GAMENET_TEST_ASSERT(observation.terminalCalls == 2);
    GAMENET_TEST_ASSERT(observation.calls == 1);
    channel->disableAll();
    channel->remove();
    std::destroy_at(channel);
}

#endif

}  // namespace

int main() {
    testCompletionVocabularyIsOperationShaped();
#ifdef _WIN32
    testNativePacketsBecomeDistinctTerminalNotices();
    testGenerationRejectsDuplicateAndRejectedSubmissionPackets();
    testObserverRevokeDoesNotRetireKernelLeaseEarly();
    testEventLoopDirectlyDispatchesAllOperationKindsWithinOwnerBudget();
    testDirectDispatchRevalidatesObserverAfterReentry();
    testSubmissionCapturesObserverBeforeNativeDequeue();
    testSameAddressObserverReplacementCannotReviveOldCompletion();
#endif
    return 0;
}
