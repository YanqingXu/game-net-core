from __future__ import annotations

import re
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing threading gate fragment in {source}: {needle}"


def parse_gamenet_tests(cmake_text: str) -> dict[str, set[str]]:
    tests: dict[str, set[str]] = {}
    pattern = re.compile(
        r"add_gamenet_test\(\s*"
        r"(?P<layer>\S+)\s+"
        r"(?P<module>\S+)\s+"
        r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/(?P<source>[^\s\)]+)"
        r"(?P<labels>[^\)]*)\)"
    )
    for match in pattern.finditer(cmake_text):
        source = match.group("source")
        labels = {match.group("layer"), match.group("module")}
        labels.update(match.group("labels").split())
        tests[source] = labels
    return tests


def require_labels(tests: dict[str, set[str]], source: str, required: set[str]) -> None:
    assert source in tests, f"missing configured test for threading gate: {source}"
    labels = tests[source]
    missing = required - labels
    assert not missing, f"{source} missing labels {sorted(missing)}; labels={sorted(labels)}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    migration_status = repo_root / "docs" / "migration_status.md"

    tests = parse_gamenet_tests(tests_cmake.read_text(encoding="utf-8"))

    required_threading_contracts = [
        "contract/event_loop/test_event_loop.cpp",
        "contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp",
        "contract/event_loop_thread_pool/test_event_loop_thread_pool_restart_soak.cpp",
        "contract/timer_queue/test_timer_queue.cpp",
        "contract/connector/test_connector_retry_stop.cpp",
        "contract/tcp_client/test_tcp_client_retry_stop_race.cpp",
        "contract/tcp_client/test_tcp_client_retry_stop_soak.cpp",
        "contract/tcp_client/test_tcp_client_stop_pending_connect.cpp",
        "contract/tcp_client/test_tcp_client_stop_pending_connect_soak.cpp",
        "contract/tcp_client/test_tcp_client_cross_thread_stop_pending_connect.cpp",
        "contract/tcp_client/test_tcp_client_stop_pending_connect_mixed_timing_soak.cpp",
        "contract/tcp_client/test_tcp_client_destroy_pending_connect.cpp",
        "contract/tcp_client/test_tcp_client_destroy_active_connection.cpp",
        "contract/tcp_client/test_tcp_client_stop_active_connection_mixed_timing_soak.cpp",
        "contract/tcp_client/test_tcp_client_cross_thread_disconnect_active.cpp",
        "contract/tcp_client/test_tcp_client_repeated_disconnect.cpp",
        "contract/tcp_client/test_tcp_client_repeated_stop.cpp",
        "contract/tcp_client/test_tcp_client_repeated_connect.cpp",
        "contract/tcp_client/test_tcp_client_cross_thread_connect.cpp",
        "contract/tcp_client/test_tcp_client_cross_thread_retry_config.cpp",
        "contract/tcp_server/test_tcp_server_stop_active_connections.cpp",
        "contract/tcp_server/test_tcp_server_stop_active_write.cpp",
        "contract/tcp_server/test_tcp_server_stop_multi_worker.cpp",
        "contract/tcp_server/test_tcp_server_stop_worker_active_write_soak.cpp",
        "contract/tcp_server/test_tcp_server_stop_from_worker_callback_soak.cpp",
        "contract/tcp_server/test_tcp_server_repeated_stop.cpp",
        "contract/tcp_server/test_tcp_server_stop_soak.cpp",
        "contract/tcp_connection/test_tcp_connection_cross_thread_send.cpp",
        "contract/tcp_connection/test_tcp_connection_cross_thread_state.cpp",
        "contract/tcp_connection/test_tcp_connection_send_after_close.cpp",
        "contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp",
        "contract/tcp_connection/test_tcp_connection_repeated_shutdown.cpp",
        "contract/tcp_connection/test_tcp_connection_cross_thread_force_close_soak.cpp",
        "contract/tcp_connection/test_tcp_connection_cross_thread_force_close_pending_write.cpp",
        "contract/tcp_connection/test_tcp_connection_force_close_pending_read.cpp",
        "contract/tcp_connection/test_tcp_connection_cross_thread_force_close_pending_read.cpp",
        "contract/tcp_connection/test_tcp_connection_force_close_pending_read_mixed_timing_soak.cpp",
        "contract/tcp_connection/test_tcp_connection_force_close_pending_write_soak.cpp",
        "contract/tcp_connection/test_tcp_connection_force_close_pending_write_mixed_timing_soak.cpp",
    ]
    for source in required_threading_contracts:
        require_labels(tests, source, {"contract", "threading", "lifecycle"})
    require_labels(
        tests,
        "contract/base/test_logger_thread_safety.cpp",
        {"contract", "threading"},
    )

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "linux-tsan:", workflow)
    require(workflow_text, "GAMENET_ENABLE_TSAN=ON", workflow)
    require(workflow_text, "ctest --test-dir build-tsan --output-on-failure -L threading --timeout 30", workflow)
    require(workflow_text, "python3 tests/cmake/test_threading_gate_contracts.py", workflow)

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "test_threading_gate_contracts.py", ci_docs)
    require(ci_docs_text, "direct `Connector::stop()` retry-timer cancellation contracts", ci_docs)
    require(ci_docs_text, "mixed-timing pending-ConnectEx `TcpClient::stop()` contracts", ci_docs)
    require(ci_docs_text, "active retry-enabled `TcpClient::stop()` after peer-close contracts", ci_docs)
    require(ci_docs_text, "active cross-thread `TcpClient::disconnect()` contracts", ci_docs)
    require(ci_docs_text, "repeated active `TcpClient::disconnect()` idempotence contracts", ci_docs)
    require(ci_docs_text, "repeated active `TcpClient::stop()` idempotence contracts", ci_docs)
    require(ci_docs_text, "repeated active `TcpClient::connect()` idempotence contracts", ci_docs)
    require(ci_docs_text, "active cross-thread `TcpClient::connect()` contracts", ci_docs)
    require(ci_docs_text, "active cross-thread `TcpClient` retry configuration contracts", ci_docs)
    require(ci_docs_text, "concurrent Logger emission/configuration contracts", ci_docs)
    require(ci_docs_text, "post-close `TcpConnection::send()` ignore contracts", ci_docs)
    require(ci_docs_text, "cross-thread `TcpConnection` state observation contracts", ci_docs)
    require(ci_docs_text, "mixed-timing pending-read `TcpConnection::forceClose()` contracts", ci_docs)
    require(ci_docs_text, "mixed-timing pending-write `TcpConnection::forceClose()` contracts", ci_docs)
    require(ci_docs_text, "repeated `TcpConnection::shutdown()` idempotence contracts", ci_docs)
    require(ci_docs_text, "worker-owned active-write `TcpServer::stop()` contracts", ci_docs)
    require(ci_docs_text, "worker-callback\n`TcpServer::stop()` contracts", ci_docs)
    require(ci_docs_text, "repeated `TcpServer::stop()` idempotence contracts", ci_docs)

    migration_text = migration_status.read_text(encoding="utf-8")
    require(migration_text, "Linux TSan race-oriented build and tests", migration_status)
    require(migration_text, "direct Connector retry-stop cancellation", migration_status)
    require(migration_text, "mixed-timing pending ConnectEx stop", migration_status)
    require(migration_text, "active retry-enabled TcpClient stop-after-peer-close", migration_status)
    require(migration_text, "active cross-thread TcpClient disconnect", migration_status)
    require(migration_text, "repeated active TcpClient disconnect idempotence", migration_status)
    require(migration_text, "repeated active TcpClient stop idempotence", migration_status)
    require(migration_text, "repeated active TcpClient connect idempotence", migration_status)
    require(migration_text, "active cross-thread TcpClient connect", migration_status)
    require(migration_text, "active cross-thread TcpClient retry configuration", migration_status)
    require(migration_text, "concurrent Logger runtime-configuration coverage", migration_status)
    require(migration_text, "post-close TcpConnection send ignore", migration_status)
    require(migration_text, "cross-thread TcpConnection state observation", migration_status)
    require(migration_text, "mixed-timing pending-read forceClose", migration_status)
    require(migration_text, "mixed-timing pending-write forceClose", migration_status)
    require(migration_text, "repeated TcpConnection shutdown idempotence", migration_status)
    require(migration_text, "worker-owned active-write stop", migration_status)
    require(migration_text, "worker-callback TcpServer stop", migration_status)
    require(migration_text, "repeated TcpServer stop idempotence", migration_status)


if __name__ == "__main__":
    main()
