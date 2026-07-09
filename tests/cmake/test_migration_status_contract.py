from __future__ import annotations

import re
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing migration status fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    migration_status = repo_root / "docs" / "migration_status.md"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    configured_tests = re.findall(r"^add_gamenet_test\((unit|contract|integration)\s", tests_cmake_text, re.MULTILINE)
    configured_test_count = len(configured_tests)
    unit_count = configured_tests.count("unit")
    contract_count = configured_tests.count("contract")
    integration_count = configured_tests.count("integration")
    assert configured_test_count > 0, "tests/CMakeLists.txt should configure CTest tests"

    status_text = migration_status.read_text(encoding="utf-8")
    require(status_text, "Last checked: 2026-07-09", migration_status)
    require(status_text, f"{configured_test_count} configured CTest tests", migration_status)
    require(
        status_text,
        f"{unit_count} unit tests, {contract_count} contract tests, and {integration_count} integration test",
        migration_status,
    )
    require(status_text, "Latest remote CI run: ci #23 for commit 9b27a0a", migration_status)
    require(status_text, "completed successfully on 2026-07-08", migration_status)
    require(status_text, "Linux CMake build and tests", migration_status)
    require(status_text, "Linux ASan/UBSan build and tests", migration_status)
    require(status_text, "Linux Release build", migration_status)
    require(status_text, "Windows MSVC IOCP build and tests", migration_status)
    require(status_text, "Linux TSan race-oriented build and tests", migration_status)
    require(status_text, "GAMENET_ENABLE_TSAN=ON", migration_status)
    require(status_text, "threading` label", migration_status)
    require(status_text, "pending read/write forceClose cancel-close", migration_status)
    require(status_text, "Race-oriented CI remote green evidence is established by ci #23 on main", migration_status)
    require(status_text, "intent consistency guard", migration_status)
    require(status_text, "TCP lifecycle contract guard", migration_status)
    require(status_text, "TcpConnection context contract guard", migration_status)
    require(status_text, "EventLoop contract guard", migration_status)
    require(status_text, "EventLoopThreadPool contract guard", migration_status)
    require(status_text, "TimerQueue contract guard", migration_status)
    require(status_text, "threading gate contract guard", migration_status)
    require(status_text, "MSVC UTF-8 build contract guard", migration_status)
    require(status_text, "Windows IOCP milestone contract guard", migration_status)
    require(status_text, "Windows IOCP data-path contract guard", migration_status)
    require(status_text, "Windows support is now represented by a `windows-msvc` workflow job", migration_status)
    require(status_text, "Windows install/package consumer gate also passes locally", migration_status)
    require(status_text, "find_package(GameNetCore)", migration_status)
    require(status_text, "GameNet::core", migration_status)
    require(status_text, "remote green status is established by ci #23 on main", migration_status)
    require(status_text, "contract.timer_queue.test_timer_queue", migration_status)
    require(status_text, "server stop with active connections", migration_status)
    require(status_text, "server stop during active write", migration_status)
    require(status_text, "server stop soak for worker-owned connections", migration_status)
    require(status_text, "server multi-worker stop from the base loop", migration_status)
    require(status_text, "server worker-owned active-write stop", migration_status)
    require(status_text, "server worker-callback TcpServer stop soak", migration_status)
    require(status_text, "server repeated stop idempotence", migration_status)
    require(status_text, "contract.tcp_server.test_tcp_server_repeated_stop", migration_status)
    require(status_text, "client retry stop race", migration_status)
    require(status_text, "client retry-stop soak", migration_status)
    require(status_text, "direct Connector retry-stop cancellation", migration_status)
    require(status_text, "contract.connector.test_connector_retry_stop", migration_status)
    require(status_text, "client stop during pending ConnectEx", migration_status)
    require(status_text, "client pending ConnectEx stop soak", migration_status)
    require(status_text, "client cross-thread stop during pending ConnectEx", migration_status)
    require(status_text, "client mixed-timing pending ConnectEx stop soak", migration_status)
    require(status_text, "client destruction during pending ConnectEx", migration_status)
    require(status_text, "client destruction with active TcpConnection", migration_status)
    require(status_text, "client mixed-timing active-connection stop soak", migration_status)
    require(status_text, "client cross-thread active disconnect", migration_status)
    require(status_text, "client repeated active disconnect idempotence", migration_status)
    require(status_text, "client repeated active stop idempotence", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_repeated_stop", migration_status)
    require(status_text, "client repeated active connect idempotence", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_repeated_connect", migration_status)
    require(status_text, "client cross-thread active connect", migration_status)
    require(status_text, "peer close convergence", migration_status)
    require(status_text, "peer reset convergence", migration_status)
    require(status_text, "error-triggered teardown idempotence", migration_status)
    require(status_text, "cross-thread send delivery", migration_status)
    require(status_text, "post-close TcpConnection send ignore", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_send_after_close", migration_status)
    require(status_text, "write-complete callback ordering", migration_status)
    require(status_text, "shutdown while output pending", migration_status)
    require(status_text, "cross-thread shutdown draining", migration_status)
    require(status_text, "repeated TcpConnection shutdown idempotence", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_repeated_shutdown", migration_status)
    require(status_text, "high-water mark notification", migration_status)
    require(status_text, "repeated forceClose idempotence", migration_status)
    require(status_text, "repeated connectDestroyed stale-registration cleanup", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed", migration_status)
    require(status_text, "cross-thread forceClose soak", migration_status)
    require(status_text, "cross-thread pending-read forceClose", migration_status)
    require(status_text, "cross-thread pending-write forceClose", migration_status)
    require(status_text, "pending-read forceClose cancellation", migration_status)
    require(status_text, "mixed-timing pending-read forceClose soak", migration_status)
    require(status_text, "pending-write forceClose soak", migration_status)
    require(status_text, "mixed-timing pending-write forceClose soak", migration_status)
    require(status_text, "TimerQueue ready-timer cancellation race", migration_status)
    require(status_text, "EventLoopThreadPool queued-work soak", migration_status)
    require(status_text, "EventLoopThreadPool restart-stop soak", migration_status)
    require(status_text, "shared tests/support helpers", migration_status)
    require(status_text, "SocketPair.h", migration_status)
    require(status_text, "ClientSocket.h", migration_status)
    require(status_text, "centralizes nonblocking test-client connect and cleanup", migration_status)
    require(status_text, "Acceptor/Socket contracts", migration_status)
    require(status_text, "TcpConnection peer-reset setup", migration_status)
    require(status_text, "TcpConnectionHarness.h", migration_status)
    require(status_text, "centralizes loop-bound TcpConnection construction", migration_status)
    require(status_text, "TcpConnectionCallbacks.h", migration_status)
    require(status_text, "LoopTest.h", migration_status)
    require(status_text, "TcpClient lifecycle watchdogs", migration_status)
    require(status_text, "TcpServer lifecycle watchdogs", migration_status)
    require(status_text, "ThreadHandoff.h", migration_status)
    require(status_text, "centralizes one-shot and delayed non-owner-thread handoff", migration_status)
    require(status_text, "FutureTest.h", migration_status)
    require(status_text, "centralizes bounded future waits for EventLoop, EventLoopThread, TimerQueue, and EventLoopThreadPool async contract tests", migration_status)
    require(status_text, "TcpClientStopHarness.h", migration_status)
    require(status_text, "centralizes retry-stop stale-reconnect assertions", migration_status)
    require(status_text, "TcpServerHarness.h", migration_status)
    require(
        status_text,
        "centralizes multi-client TcpServer connection setup and worker-loop distribution assertions",
        migration_status,
    )
    require(status_text, "Local Windows Debug long-soak also passes for the current 43-test threading slice", migration_status)
    require(status_text, "ctest --test-dir build -C Debug --output-on-failure -L threading --repeat until-fail:20 --timeout 60", migration_status)
    require(status_text, "43/43 threading-labeled tests passed across 20 repeats", migration_status)
    require(status_text, "CTest reported total test time was 637.56 seconds", migration_status)
    require(status_text, "Remote GitHub `long-soak` evidence is now recorded", migration_status)
    require(status_text, "run 28986707243", migration_status)
    require(status_text, "job 86017363504", migration_status)
    require(status_text, "commit 9b27a0a3c3993cb1f90ef4357fa80027205ca221", migration_status)
    require(status_text, "repeat 20", migration_status)
    require(status_text, "timeout 60 seconds", migration_status)
    require(status_text, "2026-07-09T01:15:38Z", migration_status)
    require(status_text, "36/36 threading-labeled tests passed", migration_status)
    require(status_text, "608.67 seconds", migration_status)
    require(status_text, "Local Windows Release", migration_status)
    require(status_text, "evidence now also passes", migration_status)
    require(status_text, "cmake --build build-release --config Release --parallel", migration_status)
    require(status_text, "ctest --test-dir build-release -C Release --output-on-failure --timeout 10", migration_status)
    require(status_text, "64/64 Release tests passed", migration_status)
    require(status_text, "install/package consumer also passes locally", migration_status)
    require(status_text, "build-release/_install", migration_status)
    require(status_text, "build-release-install-consumer", migration_status)
    require(status_text, "## Phase 4 Readiness Gate", migration_status)
    require(
        status_text,
        "Phase 4 remains deferred until every gate item below has current evidence",
        migration_status,
    )
    require(
        status_text,
        "latest HEAD has green remote CI evidence for Linux CMake, Linux ASan/UBSan, Linux TSan, Linux Release, and Windows MSVC IOCP",
        migration_status,
    )
    require(
        status_text,
        "remote `long-soak` workflow has a green run recorded with run id, commit sha, repeat count, timeout, date, and result",
        migration_status,
    )
    require(
        status_text,
        "`docs/migration_status.md`, `docs/development/ci.md`, and `docs/development/windows_iocp_milestone.md` have no pending evidence",
        migration_status,
    )
    require(
        status_text,
        "TcpConnection, TcpClient/Connector, and TcpServer lifecycle/race tests have no known flaky entries",
        migration_status,
    )
    require(
        status_text,
        "Linux and Windows install/package consumers pass through `find_package(GameNetCore)` and `GameNet::core`",
        migration_status,
    )
    require(
        status_text,
        "The first Phase 4 design-only PR should target protocol framing / PacketFramer",
        migration_status,
    )
    require(
        status_text,
        "HTTP, RPC, and game pipeline modules must stay deferred until protocol framing has its own approved intent, invariants, contracts, and tests",
        migration_status,
    )
    assert "Result: 21/21 tests passed" not in status_text, (
        "migration status must not present the old 21/21 result as the current worktree result"
    )
    assert "pending fresh Linux CI execution" not in status_text, (
        "migration status must not keep the pre-green CI state after the latest successful workflow run"
    )
    assert "remote green status is established by pull request or manual workflow execution" not in status_text, (
        "migration status must name the successful remote run instead of a future establishment path"
    )
    assert "ci #17" not in status_text, "migration status must not keep stale ci #17 as current evidence"
    assert "Remote green evidence for this new job is pending" not in status_text, (
        "migration status must not keep stale pending TSan evidence after ci #23 passed"
    )
    assert "remote `long-soak` workflow evidence remains" not in status_text, (
        "migration status must not keep stale pending long-soak evidence after run 28986707243 passed"
    )
    assert "pending until the manual workflow is run on GitHub" not in status_text, (
        "migration status must not say the manual long-soak is pending after run 28986707243 passed"
    )

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_migration_status_contract.py", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_migration_status_contract.py", ci_contract)


if __name__ == "__main__":
    main()
