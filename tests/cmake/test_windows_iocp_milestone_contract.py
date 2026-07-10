from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing Windows IOCP milestone fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    milestone = repo_root / "docs" / "development" / "windows_iocp_milestone.md"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    migration_status = repo_root / "docs" / "migration_status.md"
    data_path_spec = repo_root / "docs" / "superpowers" / "specs" / "2026-07-07-windows-iocp-data-path-design.md"
    data_path_plan = repo_root / "docs" / "superpowers" / "plans" / "2026-07-07-windows-iocp-data-path.md"
    platform_intent = repo_root / "intents" / "modules" / "platform_runtime.intent.md"
    poller_intent = repo_root / "intents" / "modules" / "poller.intent.md"
    poller_header = repo_root / "include" / "gamenet" / "core" / "net" / "Poller.h"
    event_loop_source = repo_root / "src" / "core" / "net" / "EventLoop.cc"
    iocp_header = repo_root / "include" / "gamenet" / "core" / "net" / "poller" / "IocpPoller.h"
    iocp_source = repo_root / "src" / "core" / "net" / "poller" / "IocpPoller.cc"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    assert milestone.exists(), f"missing Windows IOCP milestone document: {milestone}"
    assert data_path_spec.exists(), f"missing Windows IOCP data-path design: {data_path_spec}"
    assert data_path_plan.exists(), f"missing Windows IOCP data-path plan: {data_path_plan}"
    milestone_text = milestone.read_text(encoding="utf-8")
    require(milestone_text, "Windows IOCP Milestone", milestone)
    require(milestone_text, "Last checked: 2026-07-10", milestone)
    require(milestone_text, "`windows-msvc` workflow job", milestone)
    require(milestone_text, "WinSock `select()` is not an accepted fallback", milestone)
    require(milestone_text, "IocpPoller owns the completion port", milestone)
    require(milestone_text, "EventLoop wakeup must complete through IOCP", milestone)
    require(milestone_text, "overlapped read/write state", milestone)
    require(milestone_text, "cancel/close ordering", milestone)
    require(milestone_text, "PostQueuedCompletionStatus", milestone)
    require(milestone_text, "passes 67/67 tests", milestone)
    require(milestone_text, "0 failing tests", milestone)
    require(milestone_text, "PR `ci` run `29076601085` (#27)", milestone)
    require(milestone_text, "`86309502342`", milestone)
    require(milestone_text, "`a7fd77cbd2140041cebb3f900d5c609fafc2adad`", milestone)
    require(milestone_text, "Main `ci` run `29079836593` (#29)", milestone)
    require(milestone_text, "`c4818d4b3956c85830e04d4a1f32df4ad701d453`", milestone)
    require(milestone_text, "tag `v0.1.0-core-preview`", milestone)
    require(milestone_text, "single_get_queued_completion_status", milestone)
    require(milestone_text, "benchmark_results/2026-07-10-windows-msvc-release", milestone)
    require(milestone_text, "`29077151229`", milestone)
    require(milestone_text, "16.188/26.110 MiB/s", milestone)
    require(milestone_text, "70,848 bytes", milestone)
    require(milestone_text, "AcceptEx", milestone)
    require(milestone_text, "ConnectEx", milestone)
    require(milestone_text, "WSARecv", milestone)
    require(milestone_text, "WSASend", milestone)
    require(milestone_text, "contract.event_loop.test_event_loop", milestone)
    require(milestone_text, "contract.event_loop_thread_pool.test_event_loop_thread_pool", milestone)
    require(milestone_text, "contract.event_loop_thread_pool.test_event_loop_thread_pool_restart_soak", milestone)
    require(milestone_text, "queued-work soak", milestone)
    require(milestone_text, "contract.timer_queue.test_timer_queue", milestone)
    require(milestone_text, "ready-timer cancellation", milestone)
    require(milestone_text, "contract.acceptor.test_acceptor_contract", milestone)
    require(milestone_text, "contract.connector.test_connector_contract", milestone)
    require(milestone_text, "contract.connector.test_connector_retry_stop", milestone)
    require(milestone_text, "contract.poller.test_poller_contract", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_contract", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_retry_stop_soak", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_stop_pending_connect", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_stop_pending_connect_soak", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_cross_thread_stop_pending_connect", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_stop_pending_connect_mixed_timing_soak", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_destroy_pending_connect", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_destroy_active_connection", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_stop_active_connection_mixed_timing_soak", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_cross_thread_disconnect_active", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_repeated_disconnect", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_repeated_stop", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_repeated_connect", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_cross_thread_connect", milestone)
    require(milestone_text, "contract.tcp_client.test_tcp_client_cross_thread_retry_config", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_cross_thread_state", milestone)
    require(milestone_text, "contract.base.test_logger_thread_safety", milestone)
    require(milestone_text, "contract.tcp_server.test_tcp_server_contract", milestone)
    require(milestone_text, "contract.tcp_server.test_tcp_server_stop_active_write", milestone)
    require(milestone_text, "contract.tcp_server.test_tcp_server_stop_multi_worker", milestone)
    require(milestone_text, "contract.tcp_server.test_tcp_server_stop_worker_active_write_soak", milestone)
    require(milestone_text, "contract.tcp_server.test_tcp_server_stop_from_worker_callback_soak", milestone)
    require(milestone_text, "contract.tcp_server.test_tcp_server_repeated_stop", milestone)
    require(milestone_text, "contract.tcp_server.test_tcp_server_stop_soak", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_cross_thread_send", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_send_after_close", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_cross_thread_force_close_soak", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_write", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_cross_thread_shutdown", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_repeated_shutdown", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_force_close_pending_read", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_read", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_force_close_pending_read_mixed_timing_soak", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_force_close_pending_write_soak", milestone)
    require(milestone_text, "contract.tcp_connection.test_tcp_connection_force_close_pending_write_mixed_timing_soak", milestone)
    require(milestone_text, "integration.tcp.test_tcp_server_client_echo", milestone)
    require(milestone_text, "not by a select-style fallback", milestone)
    require(milestone_text, "2026-07-07-windows-iocp-data-path-design.md", milestone)
    require(milestone_text, "2026-07-07-windows-iocp-data-path.md", milestone)
    require(milestone_text, "Full Windows CTest is now part of the Windows MSVC workflow gate", milestone)
    require(milestone_text, "Windows install/package consumer gate is also part of the Windows MSVC", milestone)
    require(milestone_text, "The Windows MSVC workflow job must keep these gates green", milestone)
    require(milestone_text, "find_package(GameNetCore)", milestone)
    require(milestone_text, "GameNet::core", milestone)

    spec_text = data_path_spec.read_text(encoding="utf-8")
    require(spec_text, "loop-owned Windows IOCP operation layer", data_path_spec)
    require(spec_text, "owns only the completion port", data_path_spec)
    require(spec_text, "AcceptEx", data_path_spec)
    require(spec_text, "ConnectEx", data_path_spec)
    require(spec_text, "WSARecv", data_path_spec)
    require(spec_text, "WSASend", data_path_spec)
    require(spec_text, "WinSock `select()`", data_path_spec)
    require(spec_text, "accepted promoted backend", data_path_spec)
    require(spec_text, "Implementation status", data_path_spec)
    require(spec_text, "`windows-msvc` workflow job", data_path_spec)
    require(spec_text, "remote green status is established by ci #17 on main", data_path_spec)
    assert "remote green status is established by pull request or manual workflow execution" not in spec_text, (
        "Windows IOCP design must name the successful remote run instead of a future establishment path"
    )
    assert "Documentation continues to state that Windows support is deferred until the" not in spec_text, (
        "Windows IOCP design must not preserve stale deferred-support wording after the workflow gate exists"
    )

    plan_text = data_path_plan.read_text(encoding="utf-8")
    require(plan_text, "Windows IOCP Data Path Implementation Plan", data_path_plan)
    require(plan_text, "IocpOperation", data_path_plan)
    require(plan_text, "IocpTcpTransport", data_path_plan)
    require(plan_text, "contract.acceptor.test_acceptor_contract", data_path_plan)
    require(plan_text, "integration.tcp.test_tcp_server_client_echo", data_path_plan)
    require(plan_text, "Implementation status", data_path_plan)
    require(plan_text, "`windows-msvc` workflow job", data_path_plan)
    require(plan_text, "remote green status is established by ci #17 on main", data_path_plan)
    assert "remote green status is established by pull request or manual workflow execution" not in plan_text, (
        "Windows IOCP plan must name the successful remote run instead of a future establishment path"
    )

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "test_windows_iocp_milestone_contract.py", ci_docs)
    require(ci_docs_text, "windows_iocp_milestone.md", ci_docs)

    migration_text = migration_status.read_text(encoding="utf-8")
    require(migration_text, "Windows IOCP milestone contract guard", migration_status)
    require(migration_text, "Windows support is now represented by a `windows-msvc` workflow job", migration_status)
    require(migration_text, "PostQueuedCompletionStatus", migration_status)
    require(migration_text, "67/67", migration_status)
    require(migration_text, "0 failing tests", migration_status)
    require(migration_text, "AcceptEx", migration_status)
    require(migration_text, "ConnectEx", migration_status)
    require(migration_text, "WSARecv", migration_status)
    require(migration_text, "WSASend", migration_status)
    require(migration_text, "contract.event_loop.test_event_loop", migration_status)
    require(migration_text, "contract.event_loop_thread_pool.test_event_loop_thread_pool", migration_status)
    require(migration_text, "EventLoopThreadPool restart-stop soak", migration_status)
    require(migration_text, "contract.timer_queue.test_timer_queue", migration_status)
    require(migration_text, "contract.acceptor.test_acceptor_contract", migration_status)
    require(migration_text, "contract.tcp_server.test_tcp_server_contract", migration_status)
    require(migration_text, "contract.tcp_server.test_tcp_server_stop_active_write", migration_status)
    require(migration_text, "contract.tcp_server.test_tcp_server_stop_multi_worker", migration_status)
    require(migration_text, "contract.tcp_server.test_tcp_server_stop_worker_active_write_soak", migration_status)
    require(migration_text, "contract.tcp_server.test_tcp_server_stop_from_worker_callback_soak", migration_status)
    require(migration_text, "contract.tcp_server.test_tcp_server_repeated_stop", migration_status)
    require(migration_text, "contract.tcp_server.test_tcp_server_stop_soak", migration_status)
    require(migration_text, "cancel-close contracts", migration_status)
    require(migration_text, "contract.connector.test_connector_contract", migration_status)
    require(migration_text, "contract.connector.test_connector_retry_stop", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_retry_stop_soak", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_stop_pending_connect", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_stop_pending_connect_soak", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_cross_thread_stop_pending_connect", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_stop_pending_connect_mixed_timing_soak", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_destroy_pending_connect", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_destroy_active_connection", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_stop_active_connection_mixed_timing_soak", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_cross_thread_disconnect_active", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_repeated_disconnect", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_repeated_stop", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_repeated_connect", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_cross_thread_connect", migration_status)
    require(migration_text, "contract.tcp_client.test_tcp_client_cross_thread_retry_config", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_cross_thread_send", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_cross_thread_state", migration_status)
    require(migration_text, "contract.base.test_logger_thread_safety", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_send_after_close", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_cross_thread_force_close_soak", migration_status)
    require(
        migration_text,
        "contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_write",
        migration_status,
    )
    require(migration_text, "contract.tcp_connection.test_tcp_connection_cross_thread_shutdown", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_repeated_shutdown", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_read", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_force_close_pending_read_mixed_timing_soak", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_force_close_pending_write_soak", migration_status)
    require(migration_text, "contract.tcp_connection.test_tcp_connection_force_close_pending_write_mixed_timing_soak", migration_status)
    require(migration_text, "integration.tcp.test_tcp_server_client_echo", migration_status)
    require(migration_text, "Windows install/package consumer gate also passes locally", migration_status)
    require(migration_text, "find_package(GameNetCore)", migration_status)
    require(migration_text, "GameNet::core", migration_status)
    require(migration_text, "latest recorded green Windows job is `ci` #29", migration_status)
    require(migration_text, "2026-07-07-windows-iocp-data-path-design.md", migration_status)
    require(migration_text, "2026-07-07-windows-iocp-data-path.md", migration_status)

    platform_text = platform_intent.read_text(encoding="utf-8")
    require(platform_text, "docs/development/windows_iocp_milestone.md", platform_intent)

    poller_text = poller_intent.read_text(encoding="utf-8")
    require(poller_text, "docs/development/windows_iocp_milestone.md", poller_intent)

    poller_header_text = poller_header.read_text(encoding="utf-8")
    require(poller_header_text, "virtual bool wakeup();", poller_header)
    require(poller_header_text, "preserveSocketAssociation", poller_header)

    event_loop_text = event_loop_source.read_text(encoding="utf-8")
    require(event_loop_text, "poller_->wakeup()", event_loop_source)
    require(event_loop_text, "platform::writeWakeup", event_loop_source)

    iocp_header_text = iocp_header.read_text(encoding="utf-8")
    require(iocp_header_text, "bool wakeup() override", iocp_header)

    iocp_source_text = iocp_source.read_text(encoding="utf-8")
    require(iocp_source_text, "PostQueuedCompletionStatus", iocp_source)
    require(iocp_source_text, "kWakeupCompletionKey", iocp_source)
    require(iocp_source_text, "associatedFds_", iocp_source)

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_windows_iocp_milestone_contract.py", workflow)
    require(workflow_text, "windows-msvc:", workflow)
    require(workflow_text, "ctest --test-dir build-windows -C Debug", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_windows_iocp_milestone_contract.py", ci_contract)


if __name__ == "__main__":
    main()
