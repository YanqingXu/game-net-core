from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing EventLoopThreadPool contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    pool_test = repo_root / "tests" / "contract" / "event_loop_thread_pool" / "test_event_loop_thread_pool.cpp"
    pool_restart_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "event_loop_thread_pool"
        / "test_event_loop_thread_pool_restart_soak.cpp"
    )
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    pool_intent = repo_root / "intents" / "modules" / "event_loop_thread_pool.intent.md"
    migration_status = repo_root / "docs" / "migration_status.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    assert pool_test.exists(), f"missing EventLoopThreadPool contract test: {pool_test}"
    assert pool_restart_soak_test.exists(), (
        f"missing EventLoopThreadPool restart soak contract: {pool_restart_soak_test}"
    )

    pool_test_text = pool_test.read_text(encoding="utf-8")
    pool_restart_soak_text = pool_restart_soak_test.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    pool_intent_text = pool_intent.read_text(encoding="utf-8")
    migration_text = migration_status.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    ci_contract_text = ci_contract.read_text(encoding="utf-8")

    require(pool_intent_text, "cross-thread queued work reaches each published worker loop", pool_intent)
    require(pool_test_text, "event-loop-thread-pool-soak-contract", pool_test)
    require(pool_test_text, "queueInLoop", pool_test)
    require(pool_test_text, "GAMENET_TEST_ASSERT(baseExecutions.load() == 0)", pool_test)
    require(pool_test_text, "GAMENET_TEST_ASSERT(totalExecutions.load() == expectedExecutions)", pool_test)
    require(pool_restart_soak_text, "event-loop-thread-pool-restart-soak-contract", pool_restart_soak_test)
    require(pool_restart_soak_text, "constexpr int iterationCount", pool_restart_soak_test)
    require(pool_restart_soak_text, "pool.start();", pool_restart_soak_test)
    require(pool_restart_soak_text, "pool.stop();", pool_restart_soak_test)
    require(pool_restart_soak_text, "queueInLoop", pool_restart_soak_test)
    require(pool_restart_soak_text, "GAMENET_TEST_ASSERT(stoppedLoops.front() == &baseLoop)", pool_restart_soak_test)
    require(tests_cmake_text, "contract event_loop_thread_pool", tests_cmake)
    require(tests_cmake_text, "test_event_loop_thread_pool.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_event_loop_thread_pool_restart_soak.cpp threading lifecycle", tests_cmake)
    require(migration_text, "EventLoopThreadPool queued-work soak", migration_status)
    require(migration_text, "EventLoopThreadPool restart-stop soak", migration_status)

    guard_command = "python3 tests/cmake/test_event_loop_thread_pool_contracts.py"
    require(workflow_text, guard_command, workflow)
    require(ci_docs_text, "test_event_loop_thread_pool_contracts.py", ci_docs)
    require(ci_contract_text, guard_command, ci_contract)


if __name__ == "__main__":
    main()
