from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing EventLoop contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    event_loop_test = repo_root / "tests" / "contract" / "event_loop" / "test_event_loop.cpp"
    event_loop_control_test = (
        repo_root
        / "tests"
        / "contract"
        / "event_loop"
        / "test_event_loop_control_saturation.cpp"
    )
    event_loop_fair_budget_test = (
        repo_root
        / "tests"
        / "contract"
        / "event_loop"
        / "test_event_loop_fair_budget.cpp"
    )
    wakeup_coalescing_test = (
        repo_root
        / "tests"
        / "contract"
        / "event_loop"
        / "test_event_loop_wakeup_coalescing.cpp"
    )
    active_batch_test = (
        repo_root
        / "tests"
        / "contract"
        / "channel"
        / "test_channel_active_batch_lifetime.cpp"
    )
    event_loop_thread_test = (
        repo_root / "tests" / "contract" / "event_loop_thread" / "test_event_loop_thread.cpp"
    )
    future_test_helper = repo_root / "tests" / "support" / "FutureTest.h"
    event_loop_header = repo_root / "include" / "gamenet" / "core" / "net" / "EventLoop.h"
    post_result_header = (
        repo_root / "include" / "gamenet" / "core" / "net" / "PostResult.h"
    )
    event_loop_source = repo_root / "src" / "core" / "net" / "EventLoop.cc"
    control_registry = (
        repo_root / "src" / "core" / "net" / "detail" / "EventLoopControlRegistry.h"
    )
    active_batch_harness = (
        repo_root
        / "src"
        / "core"
        / "net"
        / "detail"
        / "EventLoopActiveBatchHarness.h"
    )
    channel_source = repo_root / "src" / "core" / "net" / "Channel.cc"
    connector_source = repo_root / "src" / "core" / "net" / "Connector.cc"
    timer_queue_source = repo_root / "src" / "core" / "net" / "TimerQueue.cc"
    event_loop_thread_source = repo_root / "src" / "core" / "net" / "EventLoopThread.cc"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    event_loop_intent = repo_root / "intents" / "modules" / "event_loop.intent.md"
    event_loop_thread_intent = repo_root / "intents" / "modules" / "event_loop_thread.intent.md"
    migration_status = repo_root / "docs" / "migration_status.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    assert event_loop_test.exists(), f"missing EventLoop contract test: {event_loop_test}"
    assert event_loop_control_test.exists(), (
        f"missing EventLoop control saturation contract: {event_loop_control_test}"
    )
    assert event_loop_fair_budget_test.exists(), (
        f"missing EventLoop fair-budget contract: {event_loop_fair_budget_test}"
    )
    assert wakeup_coalescing_test.exists(), (
        f"missing EventLoop wakeup coalescing contract: {wakeup_coalescing_test}"
    )
    assert active_batch_test.exists(), (
        f"missing active Channel batch lifetime contract: {active_batch_test}"
    )
    assert event_loop_thread_test.exists(), f"missing EventLoopThread contract test: {event_loop_thread_test}"
    assert future_test_helper.exists(), f"missing future wait test helper: {future_test_helper}"
    assert control_registry.exists(), (
        f"missing source-private EventLoop control registry: {control_registry}"
    )
    assert active_batch_harness.exists(), (
        f"missing source-private active Channel batch harness: {active_batch_harness}"
    )

    event_loop_test_text = event_loop_test.read_text(encoding="utf-8")
    event_loop_control_test_text = event_loop_control_test.read_text(encoding="utf-8")
    event_loop_fair_budget_test_text = event_loop_fair_budget_test.read_text(
        encoding="utf-8"
    )
    wakeup_coalescing_test_text = wakeup_coalescing_test.read_text(encoding="utf-8")
    active_batch_test_text = active_batch_test.read_text(encoding="utf-8")
    event_loop_thread_test_text = event_loop_thread_test.read_text(encoding="utf-8")
    future_test_helper_text = future_test_helper.read_text(encoding="utf-8")
    event_loop_header_text = event_loop_header.read_text(encoding="utf-8")
    post_result_header_text = post_result_header.read_text(encoding="utf-8")
    event_loop_source_text = event_loop_source.read_text(encoding="utf-8")
    control_registry_text = control_registry.read_text(encoding="utf-8")
    active_batch_harness_text = active_batch_harness.read_text(encoding="utf-8")
    channel_source_text = channel_source.read_text(encoding="utf-8")
    connector_source_text = connector_source.read_text(encoding="utf-8")
    timer_queue_source_text = timer_queue_source.read_text(encoding="utf-8")
    event_loop_thread_source_text = event_loop_thread_source.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    event_loop_intent_text = event_loop_intent.read_text(encoding="utf-8")
    event_loop_thread_intent_text = event_loop_thread_intent.read_text(encoding="utf-8")
    migration_text = migration_status.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    ci_contract_text = ci_contract.read_text(encoding="utf-8")

    require(event_loop_intent_text, "cross-thread queueInLoop wakes blocked poll", event_loop_intent)
    require(
        event_loop_intent_text,
        "cross-thread-observed pending functor execution state is atomic or synchronized",
        event_loop_intent,
    )
    require(event_loop_intent_text, "quit still drains already-queued nested functors", event_loop_intent)
    require(event_loop_intent_text, "final accepted-work drain", event_loop_intent)
    require(event_loop_intent_text, "maxControlSources", event_loop_intent)
    require(event_loop_intent_text, "PostResult::Shutdown", event_loop_intent)
    require(event_loop_intent_text, "pending mailbox bit", event_loop_intent)
    require(event_loop_intent_text, "active Channel batch", event_loop_intent)
    require(event_loop_intent_text, "maxActiveChannelsPerIteration", event_loop_intent)
    require(event_loop_intent_text, "maxTimersPerIteration", event_loop_intent)
    require(event_loop_intent_text, "maxControlCallbacksPerIteration", event_loop_intent)
    require(event_loop_thread_intent_text, "explicit stop drains accepted work", event_loop_thread_intent)
    require(event_loop_intent_text, "asynchronous callback exceptions are counted", event_loop_intent)
    require(event_loop_thread_intent_text, "must not call `std::terminate`", event_loop_thread_intent)

    require(event_loop_test_text, '#include "support/FutureTest.h"', event_loop_test)
    require(event_loop_test_text, "gamenet::test::waitUntilReady", event_loop_test)
    require(event_loop_test_text, "queueInLoop", event_loop_test)
    require(event_loop_test_text, "loop->quit();", event_loop_test)
    require(event_loop_test_text, "GAMENET_TEST_ASSERT(executor.isInOwnerThread())", event_loop_test)
    require(event_loop_test_text, "GAMENET_TEST_ASSERT(!executor.available())", event_loop_test)
    require(event_loop_test_text, "GAMENET_TEST_ASSERT(future.get() != callerThread)", event_loop_test)
    require(event_loop_test_text, "EventLoopCallbackExceptionAction::Continue", event_loop_test)
    require(event_loop_test_text, "EventLoopCallbackExceptionAction::Quit", event_loop_test)
    require(event_loop_test_text, "channel callback failure", event_loop_test)
    require(event_loop_test_text, "timer callback failure", event_loop_test)
    require(event_loop_test_text, "pending callback failure", event_loop_test)
    require(event_loop_test_text, "metric callback failure", event_loop_test)
    require(event_loop_test_text, "thread init failure", event_loop_test)
    require(event_loop_test_text, "second worker init failure", event_loop_test)
    require(
        event_loop_control_test_text,
        "control-normal-reserve-saturation",
        event_loop_control_test,
    )
    require(
        event_loop_control_test_text,
        "control-same-source-10000-coalesced",
        event_loop_control_test,
    )
    require(
        event_loop_control_test_text,
        "control-pending-does-not-rewake",
        event_loop_control_test,
    )
    require(
        event_loop_control_test_text,
        "control-draining-self-rearm-only",
        event_loop_control_test,
    )
    require(
        event_loop_control_test_text,
        "control-self-notify-non-recursive",
        event_loop_control_test,
    )
    require(
        event_loop_control_test_text,
        "control-callback-exception-isolated",
        event_loop_control_test,
    )
    require(
        event_loop_control_test_text,
        "control-notify-quit-linearization",
        event_loop_control_test,
    )
    require(
        event_loop_control_test_text,
        "mergedControlNotificationCount() == 9'999",
        event_loop_control_test,
    )
    require(
        event_loop_fair_budget_test_text,
        "testActiveBatchContinuationAndBetweenRoundInvalidation",
        event_loop_fair_budget_test,
    )
    require(
        event_loop_fair_budget_test_text,
        "testExpiredTimerBudgetYieldsToAcceptedFunctor",
        event_loop_fair_budget_test,
    )
    require(
        event_loop_fair_budget_test_text,
        "testControlBudgetYieldsToTimerAndFunctor",
        event_loop_fair_budget_test,
    )
    require(
        event_loop_fair_budget_test_text,
        "testSustainedSourcesReceiveOneServicePerRound",
        event_loop_fair_budget_test,
    )
    require(
        wakeup_coalescing_test_text,
        "testMultiProducerBurstPostsOnePacket",
        wakeup_coalescing_test,
    )
    require(
        wakeup_coalescing_test_text,
        "testProducerAroundOwnerReset(false)",
        wakeup_coalescing_test,
    )
    require(
        wakeup_coalescing_test_text,
        "testProducerAroundOwnerReset(true)",
        wakeup_coalescing_test,
    )
    require(
        wakeup_coalescing_test_text,
        "testSelfRearmAndQuitDrainTheLastPacket",
        wakeup_coalescing_test,
    )
    require(
        wakeup_coalescing_test_text,
        "physicalWakeupPacketsPosted() <",
        wakeup_coalescing_test,
    )
    require(
        active_batch_test_text,
        "testPendingPeerRemovalInvalidatesItsCapturedSlot",
        active_batch_test,
    )
    require(
        active_batch_test_text,
        "testStaleRepeatedRemoveCannotEraseSameFdReplacement",
        active_batch_test,
    )
    require(
        active_batch_test_text,
        "testRemoveReregisterStopsRemainingOldReadinessCallbacks",
        active_batch_test,
    )
    require(
        active_batch_test_text,
        "testCurrentChannelRetirementDoesNotUseSaturatedFunctorQueue",
        active_batch_test,
    )
    require(event_loop_thread_test_text, '#include "support/FutureTest.h"', event_loop_thread_test)
    require(event_loop_thread_test_text, "gamenet::test::waitUntilReady", event_loop_thread_test)
    require(event_loop_thread_test_text, "loop->queueInLoop", event_loop_thread_test)
    require(event_loop_thread_test_text, "loopThread.stop();", event_loop_thread_test)
    require(event_loop_thread_test_text, "GAMENET_TEST_ASSERT(executedFuture.get() != callerThread)", event_loop_thread_test)
    assert "std::future_status::ready" not in event_loop_test_text, (
        f"{event_loop_test} must use FutureTest.h for bounded future waits"
    )
    assert "std::future_status::ready" not in event_loop_thread_test_text, (
        f"{event_loop_thread_test} must use FutureTest.h for bounded future waits"
    )

    require(future_test_helper_text, "waitUntilReady", future_test_helper)
    require(future_test_helper_text, "std::future_status::ready", future_test_helper)
    require(event_loop_header_text, "std::atomic<bool> callingPendingFunctors_", event_loop_header)
    require(event_loop_source_text, "callingPendingFunctors_.load(std::memory_order_relaxed)", event_loop_source)
    require(event_loop_source_text, "callingPendingFunctors_.store(true, std::memory_order_relaxed)", event_loop_source)
    require(event_loop_source_text, "callingPendingFunctors_.store(false, std::memory_order_relaxed)", event_loop_source)
    require(event_loop_source_text, "bool drainingAccepted{false}", event_loop_source)
    require(event_loop_source_text, "state->accepting || state->drainingAccepted", event_loop_source)
    require(event_loop_header_text, "void setCallbackExceptionHandler", event_loop_header)
    require(event_loop_header_text, "callbackExceptionCount()", event_loop_header)
    require(event_loop_header_text, "maxControlSources", event_loop_header)
    require(event_loop_header_text, "maxActiveChannelsPerIteration", event_loop_header)
    require(event_loop_header_text, "maxTimersPerIteration", event_loop_header)
    require(event_loop_header_text, "maxControlCallbacksPerIteration", event_loop_header)
    require(event_loop_header_text, "maxIocpCompletionsPerPoll", event_loop_header)
    require(post_result_header_text, "enum class PostResult", post_result_header)
    require(event_loop_header_text, "class EventLoopControlSource", event_loop_header)
    require(event_loop_header_text, "registerControlSource", event_loop_header)
    require(event_loop_header_text, "unregisterControlSource", event_loop_header)
    require(
        event_loop_header_text,
        "friend class detail::EventLoopControlRegistry",
        event_loop_header,
    )
    event_loop_class_text = event_loop_header_text.split("class EventLoop :", maxsplit=1)[1]
    assert event_loop_class_text.index("private:") < event_loop_class_text.index(
        "registerControlSource"
    ), "EventLoop control registration must remain private"
    require(
        control_registry_text,
        "class EventLoopControlRegistry final",
        control_registry,
    )
    require(
        control_registry_text,
        "return loop.registerControlSource",
        control_registry,
    )
    require(
        active_batch_harness_text,
        "class EventLoopActiveBatchHarness final",
        active_batch_harness,
    )
    require(
        event_loop_control_test_text,
        "ControlRegistry::registerSource",
        event_loop_control_test,
    )
    require(event_loop_source_text, "PostResult EventLoopControlSource::notify() const noexcept", event_loop_source)
    require(event_loop_source_text, "if (!alreadyPending)", event_loop_source)
    require(event_loop_source_text, "void EventLoop::doControlSources()", event_loop_source)
    require(event_loop_source_text, "EventLoopCallbackSource::Control", event_loop_source)
    require(event_loop_source_text, "EventLoopCallbackSource::ChannelEvent", event_loop_source)
    require(event_loop_source_text, "EventLoopCallbackSource::PendingFunctor", event_loop_source)
    require(event_loop_source_text, "EventLoopCallbackSource::Metric", event_loop_source)
    require(event_loop_source_text, "activeChannels_[index] = nullptr", event_loop_source)
    require(event_loop_source_text, "void EventLoop::retireCurrentChannel", event_loop_source)
    require(
        connector_source_text,
        "loop_->retireCurrentChannel(std::move(removedChannel))",
        connector_source,
    )
    assert "queueInLoop([deferredChannel]" not in connector_source_text, (
        f"{connector_source} must not defer Channel destruction through pending functors"
    )
    require(channel_source_text, "eventHandling_ = false;\n        throw;", channel_source)
    require(timer_queue_source_text, "TimerQueue::ExpiredResult", timer_queue_source)
    require(timer_queue_source_text, "expired.size() < maxCount", timer_queue_source)
    require(timer_queue_source_text, "timer->canceled = true", timer_queue_source)
    require(event_loop_thread_source_text, "startupException_ = std::current_exception()", event_loop_thread_source)
    require(event_loop_thread_source_text, "std::rethrow_exception(startupException)", event_loop_thread_source)
    require(tests_cmake_text, "test_event_loop.cpp threading lifecycle", tests_cmake)
    require(
        tests_cmake_text,
        "test_channel_active_batch_lifetime.cpp lifecycle",
        tests_cmake,
    )
    require(
        tests_cmake_text,
        "test_event_loop_control_saturation.cpp threading lifecycle",
        tests_cmake,
    )
    require(
        tests_cmake_text,
        "test_event_loop_fair_budget.cpp threading lifecycle",
        tests_cmake,
    )
    require(
        tests_cmake_text,
        "test_event_loop_wakeup_coalescing.cpp threading lifecycle",
        tests_cmake,
    )
    require(tests_cmake_text, "test_event_loop_thread.cpp threading lifecycle", tests_cmake)
    require(
        migration_text,
        "EventLoop, EventLoopThread, TimerQueue, and EventLoopThreadPool async contract tests",
        migration_status,
    )

    guard_command = "python3 tests/cmake/test_event_loop_contracts.py"
    require(workflow_text, guard_command, workflow)
    require(ci_docs_text, "test_event_loop_contracts.py", ci_docs)
    require(ci_contract_text, guard_command, ci_contract)


if __name__ == "__main__":
    main()
