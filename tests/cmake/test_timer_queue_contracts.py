from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing TimerQueue contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    timer_test = repo_root / "tests" / "contract" / "timer_queue" / "test_timer_queue.cpp"
    future_test_helper = repo_root / "tests" / "support" / "FutureTest.h"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    timer_intent = repo_root / "intents" / "modules" / "timer_queue.intent.md"
    migration_status = repo_root / "docs" / "migration_status.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    assert timer_test.exists(), f"missing TimerQueue contract test: {timer_test}"
    assert future_test_helper.exists(), f"missing future wait test helper: {future_test_helper}"

    timer_test_text = timer_test.read_text(encoding="utf-8")
    future_test_helper_text = future_test_helper.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    timer_intent_text = timer_intent.read_text(encoding="utf-8")
    migration_text = migration_status.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    ci_contract_text = ci_contract.read_text(encoding="utf-8")

    require(timer_intent_text, "Can cancel race with callback execution without double-fire?", timer_intent)
    require(
        timer_intent_text,
        "canceling a ready timer from an earlier ready callback prevents the canceled",
        timer_intent,
    )
    require(timer_test_text, "timer-cancel-ready-contract", timer_test)
    require(timer_test_text, '#include "support/FutureTest.h"', timer_test)
    require(timer_test_text, "gamenet::test::waitUntilReady", timer_test)
    require(timer_test_text, "loop->cancel(*second)", timer_test)
    require(timer_test_text, "GAMENET_TEST_ASSERT(!secondFired.load())", timer_test)
    require(future_test_helper_text, "waitUntilReady", future_test_helper)
    require(future_test_helper_text, "std::future_status::ready", future_test_helper)
    require(future_test_helper_text, "GAMENET_TEST_FAIL", future_test_helper)
    require(tests_cmake_text, "contract timer_queue", tests_cmake)
    require(tests_cmake_text, "test_timer_queue.cpp threading lifecycle", tests_cmake)
    require(migration_text, "TimerQueue ready-timer cancellation race", migration_status)

    guard_command = "python3 tests/cmake/test_timer_queue_contracts.py"
    require(workflow_text, guard_command, workflow)
    require(ci_docs_text, "test_timer_queue_contracts.py", ci_docs)
    require(ci_contract_text, guard_command, ci_contract)


if __name__ == "__main__":
    main()
