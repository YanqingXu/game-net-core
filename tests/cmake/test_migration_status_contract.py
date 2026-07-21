from __future__ import annotations

import re
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing migration status fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    migration_status = repo_root / "docs" / "migration_status.md"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    configured_tests = re.findall(
        r"^add_gamenet_(?:component_)?test\((unit|contract|integration)\s",
        tests_cmake_text,
        re.MULTILINE,
    )
    configured_test_count = len(configured_tests)
    unit_count = configured_tests.count("unit")
    contract_count = configured_tests.count("contract")
    integration_count = configured_tests.count("integration")
    assert configured_test_count > 0, "tests/CMakeLists.txt should configure CTest tests"

    status_text = migration_status.read_text(encoding="utf-8")
    normalized_status_text = " ".join(status_text.split())
    require(status_text, "Last checked: 2026-07-11", migration_status)
    require(status_text, f"{configured_test_count} configured CTest tests", migration_status)
    require(
        status_text,
        f"{unit_count} unit tests, {contract_count} contract tests, and {integration_count} integration test",
        migration_status,
    )
    require(status_text, "Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`", migration_status)
    require(status_text, "CI workflow run id: `29079836593` (`ci` #29, `main`)", migration_status)
    require(status_text, "Validation date: 2026-07-10", migration_status)
    require(status_text, "Release: annotated tag `v0.1.0-core-preview`", migration_status)
    require(status_text, "Focused candidate commit `a7fd77cbd2140041cebb3f900d5c609fafc2adad`", migration_status)
    require(status_text, "`29076601085` (#27)", migration_status)
    require(status_text, "preceding audited candidate", migration_status)
    require(status_text, "`d1474b5f32e609a7d2e2648af31b45635595d304`", migration_status)
    require(status_text, "`29073362905` (#26)", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_repeated_connect", migration_status)
    require(status_text, "Linux CMake build and tests", migration_status)
    require(status_text, "Linux ASan/UBSan build and tests", migration_status)
    require(status_text, "Linux Release build", migration_status)
    require(status_text, "Windows MSVC IOCP build and tests", migration_status)
    require(status_text, "Linux TSan race-oriented build and tests", migration_status)
    require(status_text, "GAMENET_ENABLE_TSAN=ON", migration_status)
    require(status_text, "threading` label", migration_status)
    require(status_text, "pending read/write forceClose cancel-close", migration_status)
    require(status_text, "Latest recorded race-oriented CI remote green evidence is `ci` #29", migration_status)
    require(status_text, "intent consistency guard", migration_status)
    require(status_text, "intent metadata contract guard", migration_status)
    require(status_text, "all 60 formal `*.intent.md` documents", migration_status)
    require(status_text, "30 active targets", migration_status)
    require(status_text, "93 explicit verification", migration_status)
    require(status_text, "connection_backpressure_controller", migration_status)
    require(status_text, "graceful_shutdown", migration_status)
    require(status_text, "global/per-peer connection limits", migration_status)
    require(status_text, "bounded fixed-window rate rejection", migration_status)
    require(status_text, "unauthenticated close", migration_status)
    require(status_text, "Production-hardening worktree Windows MSVC Debug AddressSanitizer", migration_status)
    require(status_text, "85/85 in 47.38 seconds", migration_status)
    require(status_text, "3,050/3,050 threading", migration_status)
    require(status_text, "400/400 Pipeline/Broadcast", migration_status)
    require(status_text, "All seven fixed Windows Release IOCP", migration_status)
    require(status_text, "bound to one frozen commit", migration_status)
    require(status_text, "11 legacy source-project stage documents", migration_status)
    require(status_text, "promote_gate: never", migration_status)
    require(status_text, "Core benchmark contract guard", migration_status)
    require(status_text, "Logger thread-contract guard", migration_status)
    require(status_text, "TCP lifecycle contract guard", migration_status)
    require(status_text, "TcpConnection context contract guard", migration_status)
    require(status_text, "TcpConnection thread-contract guard", migration_status)
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
    require(status_text, "latest recorded green Windows job is `ci` #29", migration_status)
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
    require(status_text, "client cross-thread retry configuration", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_cross_thread_retry_config", migration_status)
    require(status_text, "peer close convergence", migration_status)
    require(status_text, "peer reset convergence", migration_status)
    require(status_text, "error-triggered teardown idempotence", migration_status)
    require(status_text, "cross-thread send delivery", migration_status)
    require(status_text, "concurrent Logger runtime-configuration coverage", migration_status)
    require(status_text, "contract.base.test_logger_thread_safety", migration_status)
    require(status_text, "cross-thread TcpConnection state observation", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_cross_thread_state", migration_status)
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
    require(status_text, "Local Windows Debug long-soak evidence also covers the previous", migration_status)
    require(status_text, "43-test threading slice", migration_status)
    require(status_text, "expanded the threading label to 44 tests", migration_status)
    require(status_text, "ctest --test-dir build -C Debug --output-on-failure -L threading --repeat until-fail:20 --timeout 60", migration_status)
    require(status_text, "43/43 threading-labeled tests passed across 20 repeats", migration_status)
    require(status_text, "CTest reported total test time was 637.56 seconds", migration_status)
    require(status_text, "44-test threading slice was covered once by the full Windows Debug and Release", migration_status)
    require(status_text, "then-current threading slice to 46 tests", migration_status)
    require(
        normalized_status_text,
        "passed all 46 threading tests across 5 repeats",
        migration_status,
    )
    require(status_text, "CTest reported 176.90 seconds", migration_status)
    require(
        normalized_status_text,
        "corresponding historical remote GitHub `long-soak` evidence is recorded",
        migration_status,
    )
    require(status_text, "`29077148022`", migration_status)
    require(status_text, "`86311227712`", migration_status)
    require(status_text, "`a7fd77cbd2140041cebb3f900d5c609fafc2adad`", migration_status)
    require(status_text, "repeat 50", migration_status)
    require(status_text, "timeout 60 seconds", migration_status)
    require(status_text, "2026-07-10T08:04:12Z", migration_status)
    require(status_text, "46/46 threading-labeled tests passed", migration_status)
    require(status_text, "1632.47 seconds", migration_status)
    require(status_text, "28m27s", migration_status)
    require(status_text, "Local Windows Release", migration_status)
    require(status_text, "evidence now also passes", migration_status)
    require(status_text, "cmake --build build-release --config Release --parallel", migration_status)
    require(status_text, "ctest --test-dir build-release -C Release --output-on-failure --timeout 10", migration_status)
    require(status_text, "67/67 Release tests passed", migration_status)
    require(status_text, "37.38 seconds", migration_status)
    require(status_text, "GAMENET_BUILD_BENCHMARKS", migration_status)
    require(status_text, "gamenet.core_benchmark.v1", migration_status)
    require(status_text, "51.979 MiB/s", migration_status)
    require(status_text, "73.994 MiB/s", migration_status)
    require(status_text, "71,264 bytes", migration_status)
    require(status_text, "67,465,216-byte", migration_status)
    require(status_text, "manual-only `core-benchmark` workflow", migration_status)
    require(status_text, "`29077151229`", migration_status)
    require(status_text, "`epoll_wait_batch`", migration_status)
    require(status_text, "32.337/65.234 MiB/s", migration_status)
    require(status_text, "16.188/26.110 MiB/s", migration_status)
    require(status_text, "All eight JSON files", migration_status)
    require(status_text, "install/package consumer also passes locally", migration_status)
    require(status_text, "build-release/_install", migration_status)
    require(status_text, "build-release-install-consumer", migration_status)
    require(status_text, "74/74 passed in 34.67 seconds", migration_status)
    require(status_text, "74/74 passed in 34.41 seconds", migration_status)
    require(status_text, "## Phase 4 Implementation State", migration_status)
    require(
        status_text,
        "The Phase 4 entry evidence gate was satisfied by the annotated preview tag",
        migration_status,
    )
    require(status_text, "no GitHub Release was published for that tag", migration_status)
    require(
        status_text,
        "Fresh candidate-SHA remote CI evidence is recorded for Linux CMake, Linux ASan/UBSan, Linux TSan, Linux Release, and Windows MSVC IOCP",
        migration_status,
    )
    require(
        status_text,
        "remote `long-soak` workflow has a green run recorded with run id, commit sha, repeat count, timeout, date, result, and duration",
        migration_status,
    )
    require(
        status_text,
        "`docs/migration_status.md`, `docs/development/ci.md`, and `docs/development/windows_iocp_milestone.md` have no pending evidence for the validated candidate",
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
        "Matching Release `gamenet.core_benchmark.v1` evidence is recorded for Linux",
        migration_status,
    )
    require(
        status_text,
        "PacketFramer has an approved active intent",
        migration_status,
    )
    require(
        status_text,
        "PR #2 is merged and annotated tag `v0.1.0-core-preview` points at the",
        migration_status,
    )
    require(status_text, "this was a tag-only preview, not a GitHub Release", migration_status)
    require(
        status_text,
        "HTTP, RPC, UDP/KCP, TLS, coroutine, and a formal all-in-one pipeline library",
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
    assert "Current result: Latest remote CI run:" not in status_text, (
        "migration status must distinguish latest recorded remote green evidence from latest HEAD evidence"
    )
    assert "Current HEAD:" not in status_text, (
        "migration status must use immutable validation records instead of a self-referential HEAD field"
    )
    assert "The latest HEAD has green remote CI evidence" not in status_text, (
        "migration status must not claim latest HEAD green until fresh evidence is recorded for that commit"
    )
    assert "remote green status is established by ci #23 on main" not in status_text, (
        "migration status must not present ci #23 as current-HEAD remote green evidence"
    )
    assert "remote `long-soak` workflow evidence remains" not in status_text, (
        "migration status must not keep stale pending long-soak evidence after run 28986707243 passed"
    )
    assert "pending until the manual workflow is run on GitHub" not in status_text, (
        "migration status must not say the manual long-soak is pending after run 28986707243 passed"
    )
    assert "fresh remote validation is still required" not in status_text, (
        "migration status must not retain the failed-candidate state after ci #27 passed"
    )
    assert "Linux Release JSON remains required" not in status_text, (
        "migration status must not retain local-only benchmark wording after run 29077151229 passed"
    )
    assert "not the required remote repeat-50 evidence" not in status_text, (
        "migration status must not retain pre-soak wording after run 29077148022 passed"
    )

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_migration_status_contract.py", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_migration_status_contract.py", ci_contract)

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "## Remote Evidence Boundary", ci_docs)
    require(ci_docs_text, "Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`", ci_docs)
    require(ci_docs_text, "CI workflow run id: `29079836593` (`ci` #29, `main`)", ci_docs)
    require(ci_docs_text, "Release tag: `v0.1.0-core-preview`", ci_docs)
    require(ci_docs_text, "run `29076601085` (#27)", ci_docs)
    require(ci_docs_text, "run id `29073362905`", ci_docs)
    require(ci_docs_text, "contract.tcp_client.test_tcp_client_repeated_connect", ci_docs)
    require(ci_docs_text, "run `29077148022`", ci_docs)
    require(ci_docs_text, "run `29077151229`", ci_docs)
    require(ci_docs_text, "core-benchmark-linux-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad", ci_docs)
    require(ci_docs_text, "core-benchmark-windows-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad", ci_docs)
    assert "Current main HEAD" not in ci_docs_text, (
        "CI docs must not store a self-referential current-HEAD checkpoint"
    )


if __name__ == "__main__":
    main()
