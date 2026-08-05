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
    event_loop_test = repo_root / "tests" / "contract" / "event_loop" / "test_event_loop.cpp"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    pool_intent = repo_root / "intents" / "modules" / "event_loop_thread_pool.intent.md"
    pool_source = repo_root / "src" / "core" / "net" / "EventLoopThreadPool.cc"
    pool_header = repo_root / "include" / "gamenet" / "core" / "net" / "EventLoopThreadPool.h"
    tcp_server_source = repo_root / "src" / "core" / "net" / "TcpServer.cc"
    tcp_server_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_server"
        / "test_tcp_server_contract.cpp"
    )
    thread_rules = repo_root / "rules" / "thread_affinity_rules.md"
    testing_rules = repo_root / "rules" / "testing_rules.md"
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
    event_loop_test_text = event_loop_test.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    pool_intent_text = pool_intent.read_text(encoding="utf-8")
    pool_source_text = pool_source.read_text(encoding="utf-8")
    pool_header_text = pool_header.read_text(encoding="utf-8")
    tcp_server_source_text = tcp_server_source.read_text(encoding="utf-8")
    tcp_server_test_text = tcp_server_test.read_text(encoding="utf-8")
    thread_rules_text = thread_rules.read_text(encoding="utf-8")
    testing_rules_text = testing_rules.read_text(encoding="utf-8")
    migration_text = migration_status.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    ci_contract_text = ci_contract.read_text(encoding="utf-8")

    require(pool_intent_text, "cross-thread queued work reaches each published worker loop", pool_intent)
    require(pool_test_text, '#include "support/FutureTest.h"', pool_test)
    require(pool_test_text, "event-loop-thread-pool-soak-contract", pool_test)
    require(pool_test_text, "queueInLoop", pool_test)
    require(pool_test_text, "gamenet::test::waitUntilReady", pool_test)
    require(pool_test_text, "GAMENET_TEST_ASSERT(baseExecutions.load() == 0)", pool_test)
    require(pool_test_text, "GAMENET_TEST_ASSERT(totalExecutions.load() == expectedExecutions)", pool_test)
    require(pool_test_text, "event-loop-thread-pool-least-connections-contract", pool_test)
    require(pool_test_text, "event-loop-thread-pool-queue-lag-contract", pool_test)
    require(pool_test_text, "event-loop-thread-pool-consistent-hash-contract", pool_test)
    require(pool_test_text, "event-loop-thread-pool-negative-count-contract", pool_test)
    require(pool_test_text, "event-loop-thread-pool-state-machine-contract", pool_test)
    require(pool_test_text, "wrong-thread setThreadNum", pool_test)
    require(pool_test_text, "wrong-thread getAllLoops", pool_test)
    require(pool_test_text, "repeatedStartRejected", pool_test)
    require(tcp_server_test_text, "tcp-server-thread-count-contract", tcp_server_test)
    require(pool_header_text, "EventLoopSelectionPolicy", pool_header)
    require(pool_header_text, "LeastConnections", pool_header)
    require(pool_header_text, "QueueLag", pool_header)
    require(pool_header_text, "ConsistentHash", pool_header)
    require(pool_source_text, "stableKeyHash", pool_source)
    require(pool_source_text, "mix64", pool_source)
    require(pool_source_text, "pendingFunctorLoadSnapshot", pool_source)
    assert "std::hash" not in pool_source_text, (
        f"{pool_source} must use a stable selector hash rather than implementation-defined std::hash"
    )
    require(tcp_server_source_text, "selectLoop(peerAddress)", tcp_server_source)
    require(tcp_server_source_text, "recordConnectionOpened", tcp_server_source)
    require(tcp_server_source_text, "recordConnectionClosed", tcp_server_source)
    require(pool_intent_text, "deterministic rendezvous hashing", pool_intent)
    require(pool_restart_soak_text, '#include "support/FutureTest.h"', pool_restart_soak_test)
    require(pool_restart_soak_text, "event-loop-thread-pool-restart-soak-contract", pool_restart_soak_test)
    require(pool_restart_soak_text, "constexpr int iterationCount", pool_restart_soak_test)
    require(pool_restart_soak_text, "pool.start();", pool_restart_soak_test)
    require(pool_restart_soak_text, "pool.stop();", pool_restart_soak_test)
    require(pool_restart_soak_text, "queueInLoop", pool_restart_soak_test)
    require(pool_restart_soak_text, "gamenet::test::waitUntilReady", pool_restart_soak_test)
    require(pool_restart_soak_text, "GAMENET_TEST_ASSERT(stoppedLoops.front() == &baseLoop)", pool_restart_soak_test)
    assert "std::future_status::ready" not in pool_test_text, (
        f"{pool_test} must use FutureTest.h for bounded future waits"
    )
    assert "std::future_status::ready" not in pool_restart_soak_text, (
        f"{pool_restart_soak_test} must use FutureTest.h for bounded future waits"
    )
    require(pool_source_text, "baseLoop_->assertInLoopThread();", pool_source)
    require(pool_source_text, "EventLoopThreadPool thread count must be non-negative", pool_source)
    require(pool_source_text, "EventLoopThreadPool thread count must be configured before start", pool_source)
    require(pool_source_text, "EventLoopThreadPool is already started", pool_source)
    require(pool_source_text, "thread->stop();", pool_source)
    require(pool_source_text, "catch (...) {", pool_source)
    require(pool_source_text, "loops_.clear();", pool_source)
    require(event_loop_test_text, "second worker init failure", event_loop_test)
    require(pool_intent_text, "`Idle -> Started -> Idle`", pool_intent)
    require(thread_rules_text, "`Idle -> Started -> Idle`", thread_rules)
    require(testing_rules_text, "EventLoopThreadPool negative contracts", testing_rules)
    require(tests_cmake_text, "contract event_loop_thread_pool", tests_cmake)
    require(tests_cmake_text, "test_event_loop_thread_pool.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_event_loop_thread_pool_restart_soak.cpp threading lifecycle", tests_cmake)
    require(
        migration_text,
        "M3-R2 closes the EventLoopThreadPool configuration-state contract",
        migration_status,
    )
    require(migration_text, "EventLoopThreadPool queued-work soak", migration_status)
    require(migration_text, "EventLoopThreadPool restart-stop soak", migration_status)
    require(migration_text, "round-robin, least-connections, queue-lag", migration_status)

    guard_command = "python3 tests/cmake/test_event_loop_thread_pool_contracts.py"
    require(workflow_text, guard_command, workflow)
    require(ci_docs_text, "test_event_loop_thread_pool_contracts.py", ci_docs)
    require(ci_contract_text, guard_command, ci_contract)


if __name__ == "__main__":
    main()
