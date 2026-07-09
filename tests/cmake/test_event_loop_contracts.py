from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing EventLoop contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    event_loop_test = repo_root / "tests" / "contract" / "event_loop" / "test_event_loop.cpp"
    event_loop_thread_test = (
        repo_root / "tests" / "contract" / "event_loop_thread" / "test_event_loop_thread.cpp"
    )
    future_test_helper = repo_root / "tests" / "support" / "FutureTest.h"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    event_loop_intent = repo_root / "intents" / "modules" / "event_loop.intent.md"
    event_loop_thread_intent = repo_root / "intents" / "modules" / "event_loop_thread.intent.md"
    migration_status = repo_root / "docs" / "migration_status.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    assert event_loop_test.exists(), f"missing EventLoop contract test: {event_loop_test}"
    assert event_loop_thread_test.exists(), f"missing EventLoopThread contract test: {event_loop_thread_test}"
    assert future_test_helper.exists(), f"missing future wait test helper: {future_test_helper}"

    event_loop_test_text = event_loop_test.read_text(encoding="utf-8")
    event_loop_thread_test_text = event_loop_thread_test.read_text(encoding="utf-8")
    future_test_helper_text = future_test_helper.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    event_loop_intent_text = event_loop_intent.read_text(encoding="utf-8")
    event_loop_thread_intent_text = event_loop_thread_intent.read_text(encoding="utf-8")
    migration_text = migration_status.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    ci_contract_text = ci_contract.read_text(encoding="utf-8")

    require(event_loop_intent_text, "cross-thread queueInLoop wakes blocked poll", event_loop_intent)
    require(event_loop_intent_text, "quit still drains already-queued nested functors", event_loop_intent)
    require(event_loop_thread_intent_text, "explicit stop drains accepted work", event_loop_thread_intent)

    require(event_loop_test_text, '#include "support/FutureTest.h"', event_loop_test)
    require(event_loop_test_text, "gamenet::test::waitUntilReady", event_loop_test)
    require(event_loop_test_text, "queueInLoop", event_loop_test)
    require(event_loop_test_text, "loop->quit();", event_loop_test)
    require(event_loop_test_text, "GAMENET_TEST_ASSERT(future.get() != callerThread)", event_loop_test)
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
    require(tests_cmake_text, "test_event_loop.cpp threading lifecycle", tests_cmake)
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
