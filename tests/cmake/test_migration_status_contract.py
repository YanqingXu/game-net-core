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
    require(status_text, "Last checked: 2026-07-08", migration_status)
    require(status_text, f"{configured_test_count} configured CTest tests", migration_status)
    require(
        status_text,
        f"{unit_count} unit tests, {contract_count} contract tests, and {integration_count} integration test",
        migration_status,
    )
    require(status_text, "Latest remote CI run: ci #17 for commit 4cdf589", migration_status)
    require(status_text, "completed successfully on 2026-07-07", migration_status)
    require(status_text, "Linux CMake build and tests", migration_status)
    require(status_text, "Linux ASan/UBSan build and tests", migration_status)
    require(status_text, "Linux Release build", migration_status)
    require(status_text, "Windows MSVC IOCP build and tests", migration_status)
    require(status_text, "Linux TSan race-oriented build and tests", migration_status)
    require(status_text, "GAMENET_ENABLE_TSAN=ON", migration_status)
    require(status_text, "threading` label", migration_status)
    require(status_text, "pending read/write forceClose cancel-close", migration_status)
    require(status_text, "Remote green evidence for this new", migration_status)
    require(status_text, "intent consistency guard", migration_status)
    require(status_text, "TCP lifecycle contract guard", migration_status)
    require(status_text, "TcpConnection context contract guard", migration_status)
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
    require(status_text, "remote green status is established by ci #17 on main", migration_status)
    require(status_text, "contract.timer_queue.test_timer_queue", migration_status)
    require(status_text, "server stop with active connections", migration_status)
    require(status_text, "server stop during active write", migration_status)
    require(status_text, "server stop soak for worker-owned connections", migration_status)
    require(status_text, "server multi-worker stop from the base loop", migration_status)
    require(status_text, "server worker-owned active-write stop", migration_status)
    require(status_text, "server worker-callback TcpServer stop soak", migration_status)
    require(status_text, "client retry stop race", migration_status)
    require(status_text, "client retry-stop soak", migration_status)
    require(status_text, "client stop during pending ConnectEx", migration_status)
    require(status_text, "client pending ConnectEx stop soak", migration_status)
    require(status_text, "client cross-thread stop during pending ConnectEx", migration_status)
    require(status_text, "client mixed-timing pending ConnectEx stop soak", migration_status)
    require(status_text, "client destruction during pending ConnectEx", migration_status)
    require(status_text, "client destruction with active TcpConnection", migration_status)
    require(status_text, "client mixed-timing active-connection stop soak", migration_status)
    require(status_text, "client cross-thread active disconnect", migration_status)
    require(status_text, "client cross-thread active connect", migration_status)
    require(status_text, "peer close convergence", migration_status)
    require(status_text, "peer reset convergence", migration_status)
    require(status_text, "error-triggered teardown idempotence", migration_status)
    require(status_text, "cross-thread send delivery", migration_status)
    require(status_text, "write-complete callback ordering", migration_status)
    require(status_text, "shutdown while output pending", migration_status)
    require(status_text, "cross-thread shutdown draining", migration_status)
    require(status_text, "high-water mark notification", migration_status)
    require(status_text, "repeated forceClose idempotence", migration_status)
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
    assert "Result: 21/21 tests passed" not in status_text, (
        "migration status must not present the old 21/21 result as the current worktree result"
    )
    assert "pending fresh Linux CI execution" not in status_text, (
        "migration status must not keep the pre-green CI state after the latest successful workflow run"
    )
    assert "remote green status is established by pull request or manual workflow execution" not in status_text, (
        "migration status must name the successful remote run instead of a future establishment path"
    )

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_migration_status_contract.py", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_migration_status_contract.py", ci_contract)


if __name__ == "__main__":
    main()
