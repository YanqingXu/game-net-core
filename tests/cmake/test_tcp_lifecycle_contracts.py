from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing TCP lifecycle contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    tcp_client_intent = repo_root / "intents" / "modules" / "tcp_client.intent.md"
    tcp_server_intent = repo_root / "intents" / "modules" / "tcp_server.intent.md"
    tcp_connection_intent = repo_root / "intents" / "modules" / "tcp_connection.intent.md"
    server_contract_test = (
        repo_root / "tests" / "contract" / "tcp_server" / "test_tcp_server_contract.cpp"
    )
    server_stop_test = (
        repo_root / "tests" / "contract" / "tcp_server" / "test_tcp_server_stop_active_connections.cpp"
    )
    server_stop_active_write_test = (
        repo_root / "tests" / "contract" / "tcp_server" / "test_tcp_server_stop_active_write.cpp"
    )
    server_stop_soak_test = (
        repo_root / "tests" / "contract" / "tcp_server" / "test_tcp_server_stop_soak.cpp"
    )
    server_stop_multi_worker_test = (
        repo_root / "tests" / "contract" / "tcp_server" / "test_tcp_server_stop_multi_worker.cpp"
    )
    server_stop_worker_active_write_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_server"
        / "test_tcp_server_stop_worker_active_write_soak.cpp"
    )
    server_stop_from_worker_callback_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_server"
        / "test_tcp_server_stop_from_worker_callback_soak.cpp"
    )
    server_repeated_stop_test = (
        repo_root / "tests" / "contract" / "tcp_server" / "test_tcp_server_repeated_stop.cpp"
    )
    client_contract_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_contract.cpp"
    )
    client_retry_stop_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_retry_stop_race.cpp"
    )
    client_retry_stop_soak_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_retry_stop_soak.cpp"
    )
    client_stop_pending_connect_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_stop_pending_connect.cpp"
    )
    client_stop_pending_connect_soak_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_stop_pending_connect_soak.cpp"
    )
    client_cross_thread_stop_pending_connect_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_cross_thread_stop_pending_connect.cpp"
    )
    client_stop_pending_connect_mixed_timing_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_stop_pending_connect_mixed_timing_soak.cpp"
    )
    client_destroy_pending_connect_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_destroy_pending_connect.cpp"
    )
    client_destroy_active_connection_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_destroy_active_connection.cpp"
    )
    client_stop_active_connection_mixed_timing_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_stop_active_connection_mixed_timing_soak.cpp"
    )
    client_repeated_stop_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_repeated_stop.cpp"
    )
    client_cross_thread_disconnect_active_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_cross_thread_disconnect_active.cpp"
    )
    client_repeated_disconnect_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_repeated_disconnect.cpp"
    )
    client_repeated_connect_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_repeated_connect.cpp"
    )
    client_cross_thread_connect_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_cross_thread_connect.cpp"
    )
    client_cross_thread_retry_config_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_cross_thread_retry_config.cpp"
    )
    connection_peer_close_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_peer_close.cpp"
    )
    connection_peer_reset_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_peer_reset.cpp"
    )
    acceptor_contract_test = (
        repo_root / "tests" / "contract" / "acceptor" / "test_acceptor_contract.cpp"
    )
    socket_contract_test = (
        repo_root / "tests" / "contract" / "socket" / "test_socket_contract.cpp"
    )
    connector_retry_stop_test = (
        repo_root / "tests" / "contract" / "connector" / "test_connector_retry_stop.cpp"
    )
    connection_write_complete_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_write_complete_ordering.cpp"
    )
    connection_cross_thread_send_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_cross_thread_send.cpp"
    )
    connection_send_after_close_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_send_after_close.cpp"
    )
    connection_shutdown_pending_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_shutdown_pending_output.cpp"
    )
    connection_cross_thread_shutdown_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_cross_thread_shutdown.cpp"
    )
    connection_repeated_shutdown_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_repeated_shutdown.cpp"
    )
    connection_high_water_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_high_water_mark.cpp"
    )
    connection_repeated_force_close_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_repeated_force_close.cpp"
    )
    connection_repeated_destroy_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_repeated_connect_destroyed.cpp"
    )
    connection_cross_thread_force_close_soak_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_cross_thread_force_close_soak.cpp"
    )
    connection_force_close_pending_read_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_force_close_pending_read.cpp"
    )
    connection_cross_thread_force_close_pending_read_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_cross_thread_force_close_pending_read.cpp"
    )
    connection_force_close_pending_read_mixed_timing_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_force_close_pending_read_mixed_timing_soak.cpp"
    )
    connection_force_close_pending_write_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_force_close_pending_write_soak.cpp"
    )
    connection_force_close_pending_write_mixed_timing_soak_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_force_close_pending_write_mixed_timing_soak.cpp"
    )
    connection_cross_thread_force_close_pending_write_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_cross_thread_force_close_pending_write.cpp"
    )
    connection_lifecycle_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_lifecycle.cpp"
    )
    iocp_transport_header = repo_root / "src" / "core" / "net" / "platform" / "IocpTcpTransport.h"
    iocp_transport_source = repo_root / "src" / "core" / "net" / "platform" / "IocpTcpTransport_win.cc"
    loop_test_helper = repo_root / "tests" / "support" / "LoopTest.h"
    tcp_connection_harness_helper = repo_root / "tests" / "support" / "TcpConnectionHarness.h"
    tcp_connection_callbacks_helper = repo_root / "tests" / "support" / "TcpConnectionCallbacks.h"
    socket_pair_helper = repo_root / "tests" / "support" / "SocketPair.h"
    client_socket_helper = repo_root / "tests" / "support" / "ClientSocket.h"
    tcp_server_harness_helper = repo_root / "tests" / "support" / "TcpServerHarness.h"
    thread_handoff_helper = repo_root / "tests" / "support" / "ThreadHandoff.h"
    tcp_client_stop_harness_helper = repo_root / "tests" / "support" / "TcpClientStopHarness.h"
    tcp_connection_source = repo_root / "src" / "core" / "net" / "TcpConnection.cc"
    tcp_connection_header = repo_root / "include" / "gamenet" / "core" / "net" / "TcpConnection.h"
    tcp_client_source = repo_root / "src" / "core" / "net" / "TcpClient.cc"
    tcp_client_header = repo_root / "include" / "gamenet" / "core" / "net" / "TcpClient.h"
    tcp_server_source = repo_root / "src" / "core" / "net" / "TcpServer.cc"
    connector_header = repo_root / "include" / "gamenet" / "core" / "net" / "Connector.h"
    connector_source = repo_root / "src" / "core" / "net" / "Connector.cc"
    iocp_poller_source = repo_root / "src" / "core" / "net" / "poller" / "IocpPoller.cc"

    assert server_contract_test.exists(), f"missing TCP server contract: {server_contract_test}"
    assert server_stop_test.exists(), f"missing TCP server stop active-connection contract: {server_stop_test}"
    assert server_stop_active_write_test.exists(), (
        f"missing TCP server stop active-write contract: {server_stop_active_write_test}"
    )
    assert server_stop_soak_test.exists(), f"missing TCP server stop soak contract: {server_stop_soak_test}"
    assert server_stop_multi_worker_test.exists(), (
        f"missing TCP server multi-worker stop contract: {server_stop_multi_worker_test}"
    )
    assert server_stop_worker_active_write_soak_test.exists(), (
        "missing TCP server worker-owned active-write stop soak contract: "
        f"{server_stop_worker_active_write_soak_test}"
    )
    assert client_retry_stop_test.exists(), f"missing TCP client retry-stop race contract: {client_retry_stop_test}"
    assert client_retry_stop_soak_test.exists(), (
        f"missing TCP client retry-stop soak contract: {client_retry_stop_soak_test}"
    )
    assert client_stop_pending_connect_test.exists(), (
        f"missing TCP client pending-connect stop contract: {client_stop_pending_connect_test}"
    )
    assert client_stop_pending_connect_soak_test.exists(), (
        f"missing TCP client pending-connect stop soak contract: {client_stop_pending_connect_soak_test}"
    )
    assert client_cross_thread_stop_pending_connect_test.exists(), (
        "missing TCP client cross-thread pending-connect stop contract: "
        f"{client_cross_thread_stop_pending_connect_test}"
    )
    assert client_stop_pending_connect_mixed_timing_soak_test.exists(), (
        "missing TCP client mixed-timing pending-connect stop soak contract: "
        f"{client_stop_pending_connect_mixed_timing_soak_test}"
    )
    assert client_destroy_pending_connect_test.exists(), (
        f"missing TCP client pending-connect destruction contract: {client_destroy_pending_connect_test}"
    )
    assert client_destroy_active_connection_test.exists(), (
        f"missing TCP client active-connection destruction contract: {client_destroy_active_connection_test}"
    )
    assert client_stop_active_connection_mixed_timing_soak_test.exists(), (
        "missing TCP client active-connection stop mixed-timing soak contract: "
        f"{client_stop_active_connection_mixed_timing_soak_test}"
    )
    assert client_repeated_stop_test.exists(), (
        f"missing TCP client repeated-stop contract: {client_repeated_stop_test}"
    )
    assert client_cross_thread_disconnect_active_test.exists(), (
        "missing TCP client active cross-thread disconnect contract: "
        f"{client_cross_thread_disconnect_active_test}"
    )
    assert client_repeated_disconnect_test.exists(), (
        f"missing TCP client repeated-disconnect contract: {client_repeated_disconnect_test}"
    )
    assert client_repeated_connect_test.exists(), (
        f"missing TCP client repeated-connect contract: {client_repeated_connect_test}"
    )
    assert client_cross_thread_connect_test.exists(), (
        f"missing TCP client cross-thread connect contract: {client_cross_thread_connect_test}"
    )
    assert client_cross_thread_retry_config_test.exists(), (
        "missing TCP client cross-thread retry config contract: "
        f"{client_cross_thread_retry_config_test}"
    )
    assert connection_peer_close_test.exists(), (
        f"missing TCP connection peer-close contract: {connection_peer_close_test}"
    )
    assert connection_peer_reset_test.exists(), (
        f"missing TCP connection peer-reset contract: {connection_peer_reset_test}"
    )
    assert acceptor_contract_test.exists(), f"missing Acceptor contract: {acceptor_contract_test}"
    assert socket_contract_test.exists(), f"missing Socket contract: {socket_contract_test}"
    assert connector_retry_stop_test.exists(), (
        f"missing Connector retry-stop contract: {connector_retry_stop_test}"
    )
    assert connection_write_complete_test.exists(), (
        f"missing TCP connection write-complete ordering contract: {connection_write_complete_test}"
    )
    assert connection_cross_thread_send_test.exists(), (
        f"missing TCP connection cross-thread send contract: {connection_cross_thread_send_test}"
    )
    assert connection_send_after_close_test.exists(), (
        f"missing TCP connection send-after-close contract: {connection_send_after_close_test}"
    )
    assert connection_shutdown_pending_test.exists(), (
        f"missing TCP connection shutdown-pending-output contract: {connection_shutdown_pending_test}"
    )
    assert connection_cross_thread_shutdown_test.exists(), (
        f"missing TCP connection cross-thread shutdown contract: {connection_cross_thread_shutdown_test}"
    )
    assert connection_repeated_shutdown_test.exists(), (
        f"missing TCP connection repeated shutdown contract: {connection_repeated_shutdown_test}"
    )
    assert connection_high_water_test.exists(), (
        f"missing TCP connection high-water mark contract: {connection_high_water_test}"
    )
    assert connection_repeated_force_close_test.exists(), (
        f"missing TCP connection repeated force-close contract: {connection_repeated_force_close_test}"
    )
    assert connection_repeated_destroy_test.exists(), (
        "missing TCP connection repeated connectDestroyed stale-registration contract: "
        f"{connection_repeated_destroy_test}"
    )
    assert connection_cross_thread_force_close_soak_test.exists(), (
        f"missing TCP connection cross-thread force-close soak contract: {connection_cross_thread_force_close_soak_test}"
    )
    assert connection_force_close_pending_read_test.exists(), (
        f"missing TCP connection pending-read force-close contract: {connection_force_close_pending_read_test}"
    )
    assert connection_cross_thread_force_close_pending_read_test.exists(), (
        "missing TCP connection cross-thread pending-read force-close contract: "
        f"{connection_cross_thread_force_close_pending_read_test}"
    )
    assert connection_force_close_pending_read_mixed_timing_soak_test.exists(), (
        "missing TCP connection mixed-timing pending-read force-close soak contract: "
        f"{connection_force_close_pending_read_mixed_timing_soak_test}"
    )
    assert connection_force_close_pending_write_soak_test.exists(), (
        "missing TCP connection pending-write force-close soak contract: "
        f"{connection_force_close_pending_write_soak_test}"
    )
    assert connection_force_close_pending_write_mixed_timing_soak_test.exists(), (
        "missing TCP connection mixed-timing pending-write force-close soak contract: "
        f"{connection_force_close_pending_write_mixed_timing_soak_test}"
    )
    assert connection_cross_thread_force_close_pending_write_test.exists(), (
        "missing TCP connection cross-thread pending-write force-close contract: "
        f"{connection_cross_thread_force_close_pending_write_test}"
    )
    assert server_stop_from_worker_callback_soak_test.exists(), (
        "missing TCP server worker-callback stop soak contract: "
        f"{server_stop_from_worker_callback_soak_test}"
    )
    assert server_repeated_stop_test.exists(), (
        f"missing TCP server repeated-stop idempotence contract: {server_repeated_stop_test}"
    )
    assert client_contract_test.exists(), f"missing TCP client contract: {client_contract_test}"
    assert connection_lifecycle_test.exists(), f"missing TCP connection lifecycle contract: {connection_lifecycle_test}"
    assert loop_test_helper.exists(), f"missing shared EventLoop test helper: {loop_test_helper}"
    assert tcp_connection_harness_helper.exists(), (
        f"missing shared TcpConnection construction test helper: {tcp_connection_harness_helper}"
    )
    assert tcp_connection_callbacks_helper.exists(), (
        f"missing shared TcpConnection callback test helper: {tcp_connection_callbacks_helper}"
    )
    assert socket_pair_helper.exists(), f"missing shared socket pair test helper: {socket_pair_helper}"
    assert client_socket_helper.exists(), f"missing shared client socket test helper: {client_socket_helper}"
    assert tcp_server_harness_helper.exists(), (
        f"missing shared TCP server lifecycle test helper: {tcp_server_harness_helper}"
    )
    assert thread_handoff_helper.exists(), f"missing shared thread handoff test helper: {thread_handoff_helper}"
    assert tcp_client_stop_harness_helper.exists(), (
        f"missing shared TCP client stop test helper: {tcp_client_stop_harness_helper}"
    )

    test_text = server_stop_test.read_text(encoding="utf-8")
    require(test_text, "stop-reentrant-disconnect", server_stop_test)
    require(test_text, "server.stop();", server_stop_test)
    require(test_text, "server.connectionCount() == 0", server_stop_test)
    require(test_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)", server_stop_test)
    require(test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", server_stop_test)

    server_stop_active_write_text = server_stop_active_write_test.read_text(encoding="utf-8")
    require(server_stop_active_write_text, "server-stop-active-write-contract", server_stop_active_write_test)
    require(server_stop_active_write_text, "conn->send(payload);", server_stop_active_write_test)
    require(server_stop_active_write_text, "server.stop();", server_stop_active_write_test)
    require(server_stop_active_write_text, "std::chrono::milliseconds(50)", server_stop_active_write_test)
    require(server_stop_active_write_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)", server_stop_active_write_test)
    require(server_stop_active_write_text, "GAMENET_TEST_ASSERT(server.connectionCount() == 0)", server_stop_active_write_test)
    require(server_stop_active_write_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", server_stop_active_write_test)

    server_stop_soak_text = server_stop_soak_test.read_text(encoding="utf-8")
    require(server_stop_soak_text, "server-stop-soak-contract", server_stop_soak_test)
    require(server_stop_soak_text, "constexpr int iterationCount", server_stop_soak_test)
    require(server_stop_soak_text, "server.setThreadNum(1);", server_stop_soak_test)
    require(server_stop_soak_text, "server.stop();", server_stop_soak_test)
    require(server_stop_soak_text, "baseLoop.runInLoop", server_stop_soak_test)
    require(server_stop_soak_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == 1)", server_stop_soak_test)
    require(server_stop_soak_text, "GAMENET_TEST_ASSERT(server.connectionCount() == 0)", server_stop_soak_test)

    server_stop_multi_worker_text = server_stop_multi_worker_test.read_text(encoding="utf-8")
    require(server_stop_multi_worker_text, "server-stop-multi-worker-contract", server_stop_multi_worker_test)
    require(server_stop_multi_worker_text, "server.setThreadNum(2);", server_stop_multi_worker_test)
    require(server_stop_multi_worker_text, "constexpr int clientCount", server_stop_multi_worker_test)
    require(server_stop_multi_worker_text, "server.stop();", server_stop_multi_worker_test)
    require(server_stop_multi_worker_text, "baseLoop.runInLoop", server_stop_multi_worker_test)
    require(server_stop_multi_worker_text, "workerLoopTracker.requireAtLeast(2)", server_stop_multi_worker_test)
    require(server_stop_multi_worker_text, "GAMENET_TEST_ASSERT(server.connectionCount() == 0)", server_stop_multi_worker_test)
    require(server_stop_multi_worker_text, "GAMENET_TEST_ASSERT(baseLoop.isInLoopThread())", server_stop_multi_worker_test)

    server_stop_worker_active_write_soak_text = server_stop_worker_active_write_soak_test.read_text(encoding="utf-8")
    require(
        server_stop_worker_active_write_soak_text,
        "server-stop-worker-active-write-soak",
        server_stop_worker_active_write_soak_test,
    )
    require(server_stop_worker_active_write_soak_text, "constexpr int iterationCount", server_stop_worker_active_write_soak_test)
    require(server_stop_worker_active_write_soak_text, "server.setThreadNum(2);", server_stop_worker_active_write_soak_test)
    require(server_stop_worker_active_write_soak_text, "conn->send(payload);", server_stop_worker_active_write_soak_test)
    require(server_stop_worker_active_write_soak_text, "server.stop();", server_stop_worker_active_write_soak_test)
    require(server_stop_worker_active_write_soak_text, "baseLoop.runInLoop", server_stop_worker_active_write_soak_test)
    require(
        server_stop_worker_active_write_soak_text,
        "workerLoopTracker.requireAtLeast(2)",
        server_stop_worker_active_write_soak_test,
    )
    require(
        server_stop_worker_active_write_soak_text,
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == clientCount)",
        server_stop_worker_active_write_soak_test,
    )
    require(
        server_stop_worker_active_write_soak_text,
        "GAMENET_TEST_ASSERT(server.connectionCount() == 0)",
        server_stop_worker_active_write_soak_test,
    )

    server_stop_from_worker_callback_soak_text = (
        server_stop_from_worker_callback_soak_test.read_text(encoding="utf-8")
    )
    require(
        server_stop_from_worker_callback_soak_text,
        "server-stop-from-worker-callback-soak",
        server_stop_from_worker_callback_soak_test,
    )
    require(server_stop_from_worker_callback_soak_text, "constexpr int iterationCount", server_stop_from_worker_callback_soak_test)
    require(server_stop_from_worker_callback_soak_text, "constexpr int clientCount", server_stop_from_worker_callback_soak_test)
    require(server_stop_from_worker_callback_soak_text, "server.setThreadNum(2);", server_stop_from_worker_callback_soak_test)
    require(server_stop_from_worker_callback_soak_text, "server.stop();", server_stop_from_worker_callback_soak_test)
    require(server_stop_from_worker_callback_soak_text, "conn->getLoop()->isInLoopThread()", server_stop_from_worker_callback_soak_test)
    require(server_stop_from_worker_callback_soak_text, "baseLoop.runInLoop", server_stop_from_worker_callback_soak_test)
    require(
        server_stop_from_worker_callback_soak_text,
        "workerLoopTracker.requireAtLeast(2)",
        server_stop_from_worker_callback_soak_test,
    )
    require(
        server_stop_from_worker_callback_soak_text,
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == clientCount)",
        server_stop_from_worker_callback_soak_test,
    )
    require(
        server_stop_from_worker_callback_soak_text,
        "GAMENET_TEST_ASSERT(server.connectionCount() == 0)",
        server_stop_from_worker_callback_soak_test,
    )
    require(
        server_stop_worker_active_write_soak_text,
        "GAMENET_TEST_ASSERT(baseLoop.isInLoopThread())",
        server_stop_worker_active_write_soak_test,
    )

    server_repeated_stop_text = server_repeated_stop_test.read_text(encoding="utf-8")
    require(server_repeated_stop_text, "server-repeated-stop-idempotent", server_repeated_stop_test)
    require(server_repeated_stop_text, "issueRepeatedStop();", server_repeated_stop_test)
    require(server_repeated_stop_text, "server.stop();", server_repeated_stop_test)
    require(server_repeated_stop_text, "baseLoop.runInLoop", server_repeated_stop_test)
    require(server_repeated_stop_text, "GAMENET_TEST_ASSERT(connectedCallbackCount.load() == 1)", server_repeated_stop_test)
    require(server_repeated_stop_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == 1)", server_repeated_stop_test)
    require(server_repeated_stop_text, "GAMENET_TEST_ASSERT(server.connectionCount() == 0)", server_repeated_stop_test)

    client_test_text = client_retry_stop_test.read_text(encoding="utf-8")
    require(client_test_text, "client-retry-stop-race", client_retry_stop_test)
    require(client_test_text, '#include "support/TcpClientStopHarness.h"', client_retry_stop_test)
    require(client_test_text, "gamenet::test::TcpClientStopHarness", client_retry_stop_test)
    require(client_test_text, "harness.observeConnection(conn)", client_retry_stop_test)
    require(client_test_text, "harness.markStopIssued()", client_retry_stop_test)
    require(client_test_text, "harness.markServerStartedAfterStop()", client_retry_stop_test)
    require(client_test_text, "harness.assertStopped(client)", client_retry_stop_test)
    require(client_test_text, "client.enableRetry();", client_retry_stop_test)
    require(client_test_text, "client.stop();", client_retry_stop_test)
    require(client_test_text, "server.start();", client_retry_stop_test)

    client_retry_stop_soak_text = client_retry_stop_soak_test.read_text(encoding="utf-8")
    require(client_retry_stop_soak_text, "client-retry-stop-soak", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, '#include "support/TcpClientStopHarness.h"', client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "gamenet::test::TcpClientStopHarness", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "harness.observeConnection(conn)", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "harness.markStopIssued()", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "harness.markServerStartedAfterStop()", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "harness.assertStopped(client)", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "constexpr int iterationCount", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "client.enableRetry();", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "client.stop();", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "server.start();", client_retry_stop_soak_test)

    tcp_client_stop_harness_helper_text = tcp_client_stop_harness_helper.read_text(encoding="utf-8")
    require(tcp_client_stop_harness_helper_text, "class TcpClientStopHarness", tcp_client_stop_harness_helper)
    require(tcp_client_stop_harness_helper_text, "void observeConnection", tcp_client_stop_harness_helper)
    require(tcp_client_stop_harness_helper_text, "void markStopIssued", tcp_client_stop_harness_helper)
    require(tcp_client_stop_harness_helper_text, "void markServerStartedAfterStop", tcp_client_stop_harness_helper)
    require(tcp_client_stop_harness_helper_text, "void assertStopped", tcp_client_stop_harness_helper)
    require(
        tcp_client_stop_harness_helper_text,
        "GAMENET_TEST_ASSERT(loop_.isInLoopThread())",
        tcp_client_stop_harness_helper,
    )
    require(
        tcp_client_stop_harness_helper_text,
        "GAMENET_TEST_ASSERT(!connectedAfterStop_)",
        tcp_client_stop_harness_helper,
    )

    client_stop_pending_connect_text = client_stop_pending_connect_test.read_text(encoding="utf-8")
    require(client_stop_pending_connect_text, "client-stop-pending-connect", client_stop_pending_connect_test)
    require(client_stop_pending_connect_text, "client.connect();", client_stop_pending_connect_test)
    require(client_stop_pending_connect_text, "client.stop();", client_stop_pending_connect_test)
    require(client_stop_pending_connect_text, "server.start();", client_stop_pending_connect_test)
    require(client_stop_pending_connect_text, "GAMENET_TEST_ASSERT(!connectedAfterStop)", client_stop_pending_connect_test)
    require(client_stop_pending_connect_text, "GAMENET_TEST_ASSERT(client.connection() == nullptr)", client_stop_pending_connect_test)
    require(client_stop_pending_connect_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", client_stop_pending_connect_test)

    client_stop_pending_connect_soak_text = client_stop_pending_connect_soak_test.read_text(encoding="utf-8")
    require(client_stop_pending_connect_soak_text, "client-stop-pending-connect-soak", client_stop_pending_connect_soak_test)
    require(client_stop_pending_connect_soak_text, "constexpr int iterationCount", client_stop_pending_connect_soak_test)
    require(client_stop_pending_connect_soak_text, "client.connect();", client_stop_pending_connect_soak_test)
    require(client_stop_pending_connect_soak_text, "client.stop();", client_stop_pending_connect_soak_test)
    require(client_stop_pending_connect_soak_text, "server.start();", client_stop_pending_connect_soak_test)
    require(client_stop_pending_connect_soak_text, "GAMENET_TEST_ASSERT(!connectedAfterStop)", client_stop_pending_connect_soak_test)
    require(
        client_stop_pending_connect_soak_text,
        "GAMENET_TEST_ASSERT(client.connection() == nullptr)",
        client_stop_pending_connect_soak_test,
    )
    require(client_stop_pending_connect_soak_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", client_stop_pending_connect_soak_test)

    client_cross_thread_stop_pending_connect_text = client_cross_thread_stop_pending_connect_test.read_text(
        encoding="utf-8"
    )
    require(
        client_cross_thread_stop_pending_connect_text,
        "client-cross-thread-stop-pending-connect",
        client_cross_thread_stop_pending_connect_test,
    )
    require(
        client_cross_thread_stop_pending_connect_text,
        "gamenet::test::runFromNonOwnerThread",
        client_cross_thread_stop_pending_connect_test,
    )
    require(client_cross_thread_stop_pending_connect_text, "client.connect();", client_cross_thread_stop_pending_connect_test)
    require(client_cross_thread_stop_pending_connect_text, "client.stop();", client_cross_thread_stop_pending_connect_test)
    require(client_cross_thread_stop_pending_connect_text, "server.start();", client_cross_thread_stop_pending_connect_test)
    require(
        client_cross_thread_stop_pending_connect_text,
        "GAMENET_TEST_ASSERT(!connectedAfterStop)",
        client_cross_thread_stop_pending_connect_test,
    )
    require(
        client_cross_thread_stop_pending_connect_text,
        "GAMENET_TEST_ASSERT(client.connection() == nullptr)",
        client_cross_thread_stop_pending_connect_test,
    )
    require(
        client_cross_thread_stop_pending_connect_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        client_cross_thread_stop_pending_connect_test,
    )

    client_stop_pending_connect_mixed_timing_soak_text = (
        client_stop_pending_connect_mixed_timing_soak_test.read_text(encoding="utf-8")
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "client-stop-pending-connect-mixed-timing-soak",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "constexpr int iterationCount",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "iteration % 4",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "gamenet::test::NonOwnerThreadHandoff stopper",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "gamenet::test::startNonOwnerThreadAfter",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "client.connect();",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "client.stop();",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "server.start();",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(stopIssued.load())",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(!connectedAfterStop)",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(client.connection() == nullptr)",
        client_stop_pending_connect_mixed_timing_soak_test,
    )
    require(
        client_stop_pending_connect_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        client_stop_pending_connect_mixed_timing_soak_test,
    )

    client_destroy_pending_connect_text = client_destroy_pending_connect_test.read_text(encoding="utf-8")
    require(client_destroy_pending_connect_text, "client-destroy-pending-connect", client_destroy_pending_connect_test)
    require(client_destroy_pending_connect_text, "std::unique_ptr<gamenet::net::TcpClient>", client_destroy_pending_connect_test)
    require(client_destroy_pending_connect_text, "client->connect();", client_destroy_pending_connect_test)
    require(client_destroy_pending_connect_text, "client.reset();", client_destroy_pending_connect_test)
    require(client_destroy_pending_connect_text, "server.start();", client_destroy_pending_connect_test)
    require(client_destroy_pending_connect_text, "GAMENET_TEST_ASSERT(!connectedAfterDestroy)", client_destroy_pending_connect_test)
    require(client_destroy_pending_connect_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", client_destroy_pending_connect_test)

    client_destroy_active_connection_text = client_destroy_active_connection_test.read_text(encoding="utf-8")
    require(client_destroy_active_connection_text, "client-destroy-active-connection", client_destroy_active_connection_test)
    require(client_destroy_active_connection_text, "std::unique_ptr<gamenet::net::TcpClient>", client_destroy_active_connection_test)
    require(client_destroy_active_connection_text, "server.start();", client_destroy_active_connection_test)
    require(client_destroy_active_connection_text, "client->connect();", client_destroy_active_connection_test)
    require(client_destroy_active_connection_text, "client.reset();", client_destroy_active_connection_test)
    require(client_destroy_active_connection_text, "GAMENET_TEST_ASSERT(server.connectionCount() == 0)", client_destroy_active_connection_test)
    require(client_destroy_active_connection_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", client_destroy_active_connection_test)

    client_stop_active_connection_mixed_timing_soak_text = (
        client_stop_active_connection_mixed_timing_soak_test.read_text(encoding="utf-8")
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "client-stop-active-connection-mixed-timing-soak",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "constexpr int iterationCount",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "iteration % 4",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "gamenet::test::NonOwnerThreadHandoff stopper",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "gamenet::test::startNonOwnerThreadAfter",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "server.start();",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "client.connect();",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "client.stop();",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(stopIssued.load())",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(client.connection() == nullptr)",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(server.connectionCount() == 0)",
        client_stop_active_connection_mixed_timing_soak_test,
    )
    require(
        client_stop_active_connection_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        client_stop_active_connection_mixed_timing_soak_test,
    )

    client_repeated_stop_text = client_repeated_stop_test.read_text(encoding="utf-8")
    require(client_repeated_stop_text, "client-repeated-stop-idempotent", client_repeated_stop_test)
    require(client_repeated_stop_text, "ownerStopIssued", client_repeated_stop_test)
    require(client_repeated_stop_text, "nonOwnerStopIssued", client_repeated_stop_test)
    require(client_repeated_stop_text, "client.enableRetry();", client_repeated_stop_test)
    require(client_repeated_stop_text, "client.stop();", client_repeated_stop_test)
    require(client_repeated_stop_text, "serverConnection->forceClose();", client_repeated_stop_test)
    require(client_repeated_stop_text, "gamenet::test::runFromNonOwnerThread", client_repeated_stop_test)
    require(client_repeated_stop_text, "GAMENET_TEST_ASSERT(clientDisconnectedCount == 1)", client_repeated_stop_test)
    require(client_repeated_stop_text, "GAMENET_TEST_ASSERT(serverDisconnectedCount == 1)", client_repeated_stop_test)
    require(client_repeated_stop_text, "GAMENET_TEST_ASSERT(client.connection() == nullptr)", client_repeated_stop_test)
    require(client_repeated_stop_text, "GAMENET_TEST_ASSERT(server.connectionCount() == 0)", client_repeated_stop_test)

    client_cross_thread_disconnect_active_text = client_cross_thread_disconnect_active_test.read_text(encoding="utf-8")
    require(
        client_cross_thread_disconnect_active_text,
        "client-cross-thread-disconnect-active",
        client_cross_thread_disconnect_active_test,
    )
    require(
        client_cross_thread_disconnect_active_text,
        "gamenet::test::runFromNonOwnerThread",
        client_cross_thread_disconnect_active_test,
    )
    require(client_cross_thread_disconnect_active_text, "server.start();", client_cross_thread_disconnect_active_test)
    require(client_cross_thread_disconnect_active_text, "client.connect();", client_cross_thread_disconnect_active_test)
    require(client_cross_thread_disconnect_active_text, "client.disconnect();", client_cross_thread_disconnect_active_test)
    require(
        client_cross_thread_disconnect_active_text,
        "GAMENET_TEST_ASSERT(!loop.isInLoopThread())",
        client_cross_thread_disconnect_active_test,
    )
    require(
        client_cross_thread_disconnect_active_text,
        "GAMENET_TEST_ASSERT(disconnectIssued.load())",
        client_cross_thread_disconnect_active_test,
    )
    require(
        client_cross_thread_disconnect_active_text,
        "GAMENET_TEST_ASSERT(client.connection() == nullptr)",
        client_cross_thread_disconnect_active_test,
    )
    require(
        client_cross_thread_disconnect_active_text,
        "GAMENET_TEST_ASSERT(server.connectionCount() == 0)",
        client_cross_thread_disconnect_active_test,
    )
    require(
        client_cross_thread_disconnect_active_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        client_cross_thread_disconnect_active_test,
    )

    client_repeated_disconnect_text = client_repeated_disconnect_test.read_text(encoding="utf-8")
    require(client_repeated_disconnect_text, "client-repeated-disconnect-idempotent", client_repeated_disconnect_test)
    require(client_repeated_disconnect_text, "gamenet::test::runFromNonOwnerThread", client_repeated_disconnect_test)
    require(client_repeated_disconnect_text, "client.disconnect();", client_repeated_disconnect_test)
    require(client_repeated_disconnect_text, "GAMENET_TEST_ASSERT(ownerDisconnectIssued)", client_repeated_disconnect_test)
    require(
        client_repeated_disconnect_text,
        "GAMENET_TEST_ASSERT(nonOwnerDisconnectIssued.load())",
        client_repeated_disconnect_test,
    )
    require(
        client_repeated_disconnect_text,
        "GAMENET_TEST_ASSERT(clientDisconnectedCount == 1)",
        client_repeated_disconnect_test,
    )
    require(
        client_repeated_disconnect_text,
        "GAMENET_TEST_ASSERT(serverDisconnectedCount == 1)",
        client_repeated_disconnect_test,
    )
    require(
        client_repeated_disconnect_text,
        "GAMENET_TEST_ASSERT(client.connection() == nullptr)",
        client_repeated_disconnect_test,
    )
    require(
        client_repeated_disconnect_text,
        "GAMENET_TEST_ASSERT(server.connectionCount() == 0)",
        client_repeated_disconnect_test,
    )

    client_repeated_connect_text = client_repeated_connect_test.read_text(encoding="utf-8")
    require(client_repeated_connect_text, "client-repeated-connect-is-idempotent", client_repeated_connect_test)
    require(client_repeated_connect_text, "ownerConnectIssued", client_repeated_connect_test)
    require(client_repeated_connect_text, "nonOwnerConnectIssued", client_repeated_connect_test)
    require(client_repeated_connect_text, "client.connect();", client_repeated_connect_test)
    require(client_repeated_connect_text, "gamenet::test::runFromNonOwnerThread", client_repeated_connect_test)
    require(client_repeated_connect_text, "GAMENET_TEST_ASSERT(clientConnectedCount == 1)", client_repeated_connect_test)
    require(client_repeated_connect_text, "GAMENET_TEST_ASSERT(serverConnectedCount == 1)", client_repeated_connect_test)
    require(client_repeated_connect_text, "GAMENET_TEST_ASSERT(server.connectionCount() == 0)", client_repeated_connect_test)

    connector_retry_stop_text = connector_retry_stop_test.read_text(encoding="utf-8")
    require(connector_retry_stop_text, "connector-retry-stop-cancels-pending-retry", connector_retry_stop_test)
    require(connector_retry_stop_text, "ConnectorOptions options", connector_retry_stop_test)
    require(connector_retry_stop_text, "options.enableRetry = true", connector_retry_stop_test)
    require(connector_retry_stop_text, "ConnectorEvent::RetryScheduled", connector_retry_stop_test)
    require(connector_retry_stop_text, "options.connectTimeout =", connector_retry_stop_test)
    require(connector_retry_stop_text, "connector->stop();", connector_retry_stop_test)
    require(connector_retry_stop_text, "acceptor.listen();", connector_retry_stop_test)
    require(connector_retry_stop_text, "GAMENET_TEST_ASSERT(!connectedAfterStop)", connector_retry_stop_test)
    require(connector_retry_stop_text, "GAMENET_TEST_ASSERT(!acceptedAfterStop)", connector_retry_stop_test)
    require(
        connector_retry_stop_text,
        "GAMENET_TEST_ASSERT(connector->state() == gamenet::net::Connector::kDisconnected)",
        connector_retry_stop_test,
    )

    client_cross_thread_connect_text = client_cross_thread_connect_test.read_text(encoding="utf-8")
    require(client_cross_thread_connect_text, "client-cross-thread-connect", client_cross_thread_connect_test)
    require(client_cross_thread_connect_text, "gamenet::test::runFromNonOwnerThread", client_cross_thread_connect_test)
    require(client_cross_thread_connect_text, "server.start();", client_cross_thread_connect_test)
    require(client_cross_thread_connect_text, "client.connect();", client_cross_thread_connect_test)
    require(client_cross_thread_connect_text, "conn->forceClose();", client_cross_thread_connect_test)
    require(
        client_cross_thread_connect_text,
        "GAMENET_TEST_ASSERT(!loop.isInLoopThread())",
        client_cross_thread_connect_test,
    )
    require(
        client_cross_thread_connect_text,
        "GAMENET_TEST_ASSERT(connectIssued.load())",
        client_cross_thread_connect_test,
    )
    require(
        client_cross_thread_connect_text,
        "GAMENET_TEST_ASSERT(client.connection() == conn)",
        client_cross_thread_connect_test,
    )
    require(
        client_cross_thread_connect_text,
        "GAMENET_TEST_ASSERT(client.connection() == nullptr)",
        client_cross_thread_connect_test,
    )
    require(
        client_cross_thread_connect_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        client_cross_thread_connect_test,
    )

    client_cross_thread_retry_config_text = client_cross_thread_retry_config_test.read_text(encoding="utf-8")
    require(
        client_cross_thread_retry_config_text,
        "client-cross-thread-retry-config",
        client_cross_thread_retry_config_test,
    )
    require(
        client_cross_thread_retry_config_text,
        "gamenet::test::runFromNonOwnerThread",
        client_cross_thread_retry_config_test,
    )
    require(client_cross_thread_retry_config_text, "client.enableRetry();", client_cross_thread_retry_config_test)
    require(client_cross_thread_retry_config_text, "client.disableRetry();", client_cross_thread_retry_config_test)
    require(client_cross_thread_retry_config_text, "serverConnection->forceClose();", client_cross_thread_retry_config_test)
    require(
        client_cross_thread_retry_config_text,
        "GAMENET_TEST_ASSERT(!loop.isInLoopThread())",
        client_cross_thread_retry_config_test,
    )
    require(
        client_cross_thread_retry_config_text,
        "GAMENET_TEST_ASSERT(!client.retry())",
        client_cross_thread_retry_config_test,
    )
    require(
        client_cross_thread_retry_config_text,
        "GAMENET_TEST_ASSERT(clientConnectedCount == 1)",
        client_cross_thread_retry_config_test,
    )
    require(
        client_cross_thread_retry_config_text,
        "GAMENET_TEST_ASSERT(serverConnectedCount == 1)",
        client_cross_thread_retry_config_test,
    )

    peer_close_test_text = connection_peer_close_test.read_text(encoding="utf-8")
    require(peer_close_test_text, "peer-close-converges-once", connection_peer_close_test)
    require(peer_close_test_text, "closePeer();", connection_peer_close_test)
    require(peer_close_test_text, "GAMENET_TEST_ASSERT(closeCallbackCount == 1)", connection_peer_close_test)
    require(peer_close_test_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)", connection_peer_close_test)
    require(peer_close_test_text, "conn->connectDestroyed();", connection_peer_close_test)

    peer_reset_test_text = connection_peer_reset_test.read_text(encoding="utf-8")
    require(peer_reset_test_text, "peer-reset-error-converges-once", connection_peer_reset_test)
    require(peer_reset_test_text, "resetPeer();", connection_peer_reset_test)
    require(peer_reset_test_text, "conn->forceClose();", connection_peer_reset_test)
    require(peer_reset_test_text, "GAMENET_TEST_ASSERT(closeCallbackCount == 1)", connection_peer_reset_test)
    require(peer_reset_test_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)", connection_peer_reset_test)
    require(peer_reset_test_text, "conn->connectDestroyed();", connection_peer_reset_test)
    require(peer_reset_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_peer_reset_test)

    write_complete_test_text = connection_write_complete_test.read_text(encoding="utf-8")
    require(write_complete_test_text, "write-complete-after-send-return", connection_write_complete_test)
    require(write_complete_test_text, "conn->send(payload);", connection_write_complete_test)
    require(write_complete_test_text, "GAMENET_TEST_ASSERT(!writeCompleteBeforeSendReturned)", connection_write_complete_test)
    require(write_complete_test_text, "GAMENET_TEST_ASSERT(writeCompleteAfterSendReturned)", connection_write_complete_test)
    require(write_complete_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_write_complete_test)

    cross_thread_send_test_text = connection_cross_thread_send_test.read_text(encoding="utf-8")
    require(cross_thread_send_test_text, "cross-thread-send-marshals-to-owner-loop", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "gamenet::test::runFromNonOwnerThread", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "conn->send(payload);", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "GAMENET_TEST_ASSERT(peerReadBytes == payload.size())", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_cross_thread_send_test)

    send_after_close_test_text = connection_send_after_close_test.read_text(encoding="utf-8")
    require(send_after_close_test_text, "send-after-close-is-ignored", connection_send_after_close_test)
    require(send_after_close_test_text, "ownerSendAfterCloseIssued", connection_send_after_close_test)
    require(send_after_close_test_text, "nonOwnerSendAfterCloseIssued", connection_send_after_close_test)
    require(send_after_close_test_text, "conn->send(payload);", connection_send_after_close_test)
    require(send_after_close_test_text, "gamenet::test::runFromNonOwnerThread", connection_send_after_close_test)
    require(send_after_close_test_text, "GAMENET_TEST_ASSERT(peerReadBytes == 0)", connection_send_after_close_test)
    require(send_after_close_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 0)", connection_send_after_close_test)
    require(send_after_close_test_text, "GAMENET_TEST_ASSERT(connection->disconnected())", connection_send_after_close_test)

    shutdown_pending_test_text = connection_shutdown_pending_test.read_text(encoding="utf-8")
    require(shutdown_pending_test_text, "shutdown-waits-for-pending-output", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "conn->send(payload);", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "conn->shutdown();", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(peerSawEof)", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_shutdown_pending_test)

    cross_thread_shutdown_test_text = connection_cross_thread_shutdown_test.read_text(encoding="utf-8")
    require(cross_thread_shutdown_test_text, "cross-thread-shutdown-drains-output", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "gamenet::test::runFromNonOwnerThread", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "conn->shutdown();", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(peerSawEof)", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(closeCallbackCount == 1)", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_cross_thread_shutdown_test)

    repeated_shutdown_test_text = connection_repeated_shutdown_test.read_text(encoding="utf-8")
    require(repeated_shutdown_test_text, "repeated-shutdown-drains-once", connection_repeated_shutdown_test)
    require(repeated_shutdown_test_text, "gamenet::test::runFromNonOwnerThread", connection_repeated_shutdown_test)
    require(repeated_shutdown_test_text, "conn->shutdown();", connection_repeated_shutdown_test)
    require(repeated_shutdown_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_repeated_shutdown_test)
    require(repeated_shutdown_test_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)", connection_repeated_shutdown_test)
    require(repeated_shutdown_test_text, "GAMENET_TEST_ASSERT(peerSawEof)", connection_repeated_shutdown_test)
    require(repeated_shutdown_test_text, "GAMENET_TEST_ASSERT(closeCallbackCount == 1)", connection_repeated_shutdown_test)

    high_water_test_text = connection_high_water_test.read_text(encoding="utf-8")
    require(high_water_test_text, "high-water-mark-fires-on-owner-loop", connection_high_water_test)
    require(high_water_test_text, "setHighWaterMarkCallback", connection_high_water_test)
    require(high_water_test_text, "conn->send(payload);", connection_high_water_test)
    require(high_water_test_text, "GAMENET_TEST_ASSERT(highWaterCallbackCount == 1)", connection_high_water_test)
    require(high_water_test_text, "GAMENET_TEST_ASSERT(highWaterBytes >= highWaterMark)", connection_high_water_test)
    require(high_water_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_high_water_test)

    repeated_force_close_text = connection_repeated_force_close_test.read_text(encoding="utf-8")
    require(repeated_force_close_text, "repeated-force-close-is-idempotent", connection_repeated_force_close_test)
    require(repeated_force_close_text, "conn->forceClose();", connection_repeated_force_close_test)
    require(repeated_force_close_text, "assertSingleConnectDisconnectClose(callbacks)", connection_repeated_force_close_test)
    require(repeated_force_close_text, "conn->connectDestroyed();", connection_repeated_force_close_test)

    repeated_destroy_text = connection_repeated_destroy_test.read_text(encoding="utf-8")
    require(
        repeated_destroy_text,
        "repeated-connect-destroyed-removes-registration",
        connection_repeated_destroy_test,
    )
    require(repeated_destroy_text, "conn->forceClose();", connection_repeated_destroy_test)
    require(
        repeated_destroy_text,
        "conn->connectDestroyed();\n        conn->connectDestroyed();",
        connection_repeated_destroy_test,
    )
    require(repeated_destroy_text, "GAMENET_TEST_ASSERT(messageCallbackCount == 0)", connection_repeated_destroy_test)
    require(repeated_destroy_text, "connection.reset();", connection_repeated_destroy_test)
    require(repeated_destroy_text, "assertSingleConnectDisconnectClose(callbacks)", connection_repeated_destroy_test)

    cross_thread_force_close_soak_text = connection_cross_thread_force_close_soak_test.read_text(encoding="utf-8")
    require(
        cross_thread_force_close_soak_text,
        "cross-thread-force-close-soak-contract",
        connection_cross_thread_force_close_soak_test,
    )
    require(cross_thread_force_close_soak_text, "constexpr int iterationCount", connection_cross_thread_force_close_soak_test)
    require(cross_thread_force_close_soak_text, "gamenet::test::runFromNonOwnerThread", connection_cross_thread_force_close_soak_test)
    require(cross_thread_force_close_soak_text, "conn->forceClose();", connection_cross_thread_force_close_soak_test)
    require(
        cross_thread_force_close_soak_text,
        "assertSingleConnectDisconnectClose(callbacks)",
        connection_cross_thread_force_close_soak_test,
    )
    require(cross_thread_force_close_soak_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_cross_thread_force_close_soak_test)

    force_close_pending_read_text = connection_force_close_pending_read_test.read_text(encoding="utf-8")
    require(
        force_close_pending_read_text,
        "force-close-cancels-pending-read-before-destroy",
        connection_force_close_pending_read_test,
    )
    require(force_close_pending_read_text, "conn->connectEstablished();", connection_force_close_pending_read_test)
    require(force_close_pending_read_text, "conn->forceClose();", connection_force_close_pending_read_test)
    require(force_close_pending_read_text, "conn->connectDestroyed();", connection_force_close_pending_read_test)
    require(force_close_pending_read_text, "connection.reset();", connection_force_close_pending_read_test)
    require(force_close_pending_read_text, "assertSingleConnectDisconnectClose(callbacks)", connection_force_close_pending_read_test)
    require(force_close_pending_read_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_force_close_pending_read_test)

    cross_thread_force_close_pending_read_text = connection_cross_thread_force_close_pending_read_test.read_text(
        encoding="utf-8"
    )
    require(
        cross_thread_force_close_pending_read_text,
        "cross-thread-force-close-pending-read-contract",
        connection_cross_thread_force_close_pending_read_test,
    )
    require(cross_thread_force_close_pending_read_text, "constexpr int iterationCount", connection_cross_thread_force_close_pending_read_test)
    require(
        cross_thread_force_close_pending_read_text,
        "gamenet::test::runFromNonOwnerThread",
        connection_cross_thread_force_close_pending_read_test,
    )
    require(cross_thread_force_close_pending_read_text, "conn->connectEstablished();", connection_cross_thread_force_close_pending_read_test)
    require(cross_thread_force_close_pending_read_text, "conn->forceClose();", connection_cross_thread_force_close_pending_read_test)
    require(cross_thread_force_close_pending_read_text, "conn->connectDestroyed();", connection_cross_thread_force_close_pending_read_test)
    require(cross_thread_force_close_pending_read_text, "connection.reset();", connection_cross_thread_force_close_pending_read_test)
    require(
        cross_thread_force_close_pending_read_text,
        "assertSingleConnectDisconnectClose(callbacks)",
        connection_cross_thread_force_close_pending_read_test,
    )
    require(
        cross_thread_force_close_pending_read_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        connection_cross_thread_force_close_pending_read_test,
    )

    force_close_pending_read_mixed_timing_soak_text = (
        connection_force_close_pending_read_mixed_timing_soak_test.read_text(encoding="utf-8")
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "force-close-pending-read-mixed-timing-soak",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "constexpr int iterationCount",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "iteration % 4",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "gamenet::test::NonOwnerThreadHandoff closer",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "gamenet::test::startNonOwnerThreadAfter",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "conn->connectEstablished();",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "conn->forceClose();",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(forceCloseIssued.load())",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "assertSingleConnectDisconnectClose(callbacks)",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )

    force_close_pending_write_soak_text = connection_force_close_pending_write_soak_test.read_text(encoding="utf-8")
    require(
        force_close_pending_write_soak_text,
        "force-close-pending-write-soak-contract",
        connection_force_close_pending_write_soak_test,
    )
    require(force_close_pending_write_soak_text, "constexpr int iterationCount", connection_force_close_pending_write_soak_test)
    require(force_close_pending_write_soak_text, "conn->send(payload);", connection_force_close_pending_write_soak_test)
    require(force_close_pending_write_soak_text, "conn->forceClose();", connection_force_close_pending_write_soak_test)
    require(
        force_close_pending_write_soak_text,
        "assertSingleConnectDisconnectClose(callbacks)",
        connection_force_close_pending_write_soak_test,
    )
    require(
        force_close_pending_write_soak_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        connection_force_close_pending_write_soak_test,
    )

    force_close_pending_write_mixed_timing_soak_text = (
        connection_force_close_pending_write_mixed_timing_soak_test.read_text(encoding="utf-8")
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "force-close-pending-write-mixed-timing-soak",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "constexpr int iterationCount",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "iteration % 4",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "gamenet::test::NonOwnerThreadHandoff closer",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "gamenet::test::startNonOwnerThreadAfter",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "conn->send(payload);",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "conn->forceClose();",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(forceCloseIssued.load())",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "assertSingleConnectDisconnectClose(callbacks)",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )

    cross_thread_force_close_pending_write_text = connection_cross_thread_force_close_pending_write_test.read_text(
        encoding="utf-8"
    )
    require(
        cross_thread_force_close_pending_write_text,
        "cross-thread-force-close-pending-write-contract",
        connection_cross_thread_force_close_pending_write_test,
    )
    require(
        cross_thread_force_close_pending_write_text,
        "gamenet::test::runFromNonOwnerThread",
        connection_cross_thread_force_close_pending_write_test,
    )
    require(cross_thread_force_close_pending_write_text, "conn->send(payload);", connection_cross_thread_force_close_pending_write_test)
    require(cross_thread_force_close_pending_write_text, "conn->forceClose();", connection_cross_thread_force_close_pending_write_test)
    require(
        cross_thread_force_close_pending_write_text,
        "assertSingleConnectDisconnectClose(callbacks)",
        connection_cross_thread_force_close_pending_write_test,
    )
    require(
        cross_thread_force_close_pending_write_text,
        "GAMENET_TEST_ASSERT(loop.isInLoopThread())",
        connection_cross_thread_force_close_pending_write_test,
    )

    for socketpair_test in (
        connection_lifecycle_test,
        connection_peer_close_test,
        connection_peer_reset_test,
        connection_write_complete_test,
        connection_cross_thread_send_test,
        connection_shutdown_pending_test,
        connection_cross_thread_shutdown_test,
        connection_repeated_shutdown_test,
        connection_high_water_test,
        connection_repeated_force_close_test,
        connection_cross_thread_force_close_soak_test,
        connection_send_after_close_test,
        connection_force_close_pending_read_test,
        connection_cross_thread_force_close_pending_read_test,
        connection_force_close_pending_read_mixed_timing_soak_test,
        connection_force_close_pending_write_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        socketpair_text = socketpair_test.read_text(encoding="utf-8")
        require(socketpair_text, '#include "gamenet/core/net/SocketTypes.h"', socketpair_test)
        if socketpair_test == connection_peer_reset_test:
            continue
        require(socketpair_text, '#include "support/SocketPair.h"', socketpair_test)
        require(socketpair_text, "gamenet::test::ConnectedSocketPair pair", socketpair_test)

    socket_pair_helper_text = socket_pair_helper.read_text(encoding="utf-8")
    require(socket_pair_helper_text, "enum class SocketPairMode", socket_pair_helper)
    require(socket_pair_helper_text, "ConnectedSocketPair", socket_pair_helper)
    require(socket_pair_helper_text, "setNonBlockingForTest(connectionFd);", socket_pair_helper)
    require(socket_pair_helper_text, "setNonBlockingForTest(peerFd);", socket_pair_helper)
    require(socket_pair_helper_text, "setSmallSendBufferForTest(connectionFd);", socket_pair_helper)

    tcp_connection_harness_text = tcp_connection_harness_helper.read_text(encoding="utf-8")
    require(tcp_connection_harness_text, "makeTcpConnection", tcp_connection_harness_helper)
    require(tcp_connection_harness_text, "ConnectedSocketPair& pair", tcp_connection_harness_helper)
    require(tcp_connection_harness_text, "gamenet::net::sockets::getLocalAddr(pair.connectionFd)", tcp_connection_harness_helper)
    require(tcp_connection_harness_text, "gamenet::net::sockets::getPeerAddr(pair.connectionFd)", tcp_connection_harness_helper)

    client_socket_helper_text = client_socket_helper.read_text(encoding="utf-8")
    require(client_socket_helper_text, "connectTestClient", client_socket_helper)
    require(client_socket_helper_text, "closeTestSocket", client_socket_helper)
    require(client_socket_helper_text, "closeTestSockets", client_socket_helper)
    require(client_socket_helper_text, "gamenet::net::sockets::isInProgress(error)", client_socket_helper)
    require(client_socket_helper_text, "gamenet::net::sockets::isWouldBlock(error)", client_socket_helper)

    tcp_server_harness_helper_text = tcp_server_harness_helper.read_text(encoding="utf-8")
    require(tcp_server_harness_helper_text, "class WorkerLoopTracker", tcp_server_harness_helper)
    require(tcp_server_harness_helper_text, "void recordCurrentThread", tcp_server_harness_helper)
    require(tcp_server_harness_helper_text, "void requireAtLeast", tcp_server_harness_helper)
    require(tcp_server_harness_helper_text, "connectTestClients", tcp_server_harness_helper)

    tcp_connection_callbacks_helper_text = tcp_connection_callbacks_helper.read_text(encoding="utf-8")
    require(tcp_connection_callbacks_helper_text, "struct TcpConnectionCallbackCounts", tcp_connection_callbacks_helper)
    require(tcp_connection_callbacks_helper_text, "setCountingConnectionCallback", tcp_connection_callbacks_helper)
    require(tcp_connection_callbacks_helper_text, "assertSingleConnectDisconnectClose", tcp_connection_callbacks_helper)

    loop_test_helper_text = loop_test_helper.read_text(encoding="utf-8")
    require(loop_test_helper_text, "runLoopWithTimeout", loop_test_helper)
    require(loop_test_helper_text, "GAMENET_TEST_FAIL(message)", loop_test_helper)

    thread_handoff_helper_text = thread_handoff_helper.read_text(encoding="utf-8")
    require(thread_handoff_helper_text, "class NonOwnerThreadHandoff", thread_handoff_helper)
    require(thread_handoff_helper_text, "startNonOwnerThread", thread_handoff_helper)
    require(thread_handoff_helper_text, "startNonOwnerThreadAfter", thread_handoff_helper)
    require(thread_handoff_helper_text, "runFromNonOwnerThread", thread_handoff_helper)
    require(thread_handoff_helper_text, "std::thread handoff", thread_handoff_helper)
    require(thread_handoff_helper_text, "handoff.join();", thread_handoff_helper)

    for counting_callback_test in (
        connection_repeated_force_close_test,
        connection_cross_thread_force_close_soak_test,
        connection_force_close_pending_read_test,
        connection_cross_thread_force_close_pending_read_test,
        connection_force_close_pending_read_mixed_timing_soak_test,
        connection_force_close_pending_write_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        counting_callback_text = counting_callback_test.read_text(encoding="utf-8")
        require(counting_callback_text, '#include "support/TcpConnectionCallbacks.h"', counting_callback_test)
        require(counting_callback_text, "gamenet::test::TcpConnectionCallbackCounts callbacks", counting_callback_test)
        require(counting_callback_text, "gamenet::test::setCountingConnectionCallback", counting_callback_test)
        require(counting_callback_text, "gamenet::test::assertSingleConnectDisconnectClose(callbacks)", counting_callback_test)

    for tcp_connection_harness_test in (
        connection_lifecycle_test,
        connection_peer_close_test,
        connection_write_complete_test,
        connection_cross_thread_send_test,
        connection_shutdown_pending_test,
        connection_cross_thread_shutdown_test,
        connection_repeated_shutdown_test,
        connection_high_water_test,
        connection_repeated_force_close_test,
        connection_repeated_destroy_test,
        connection_cross_thread_force_close_soak_test,
        connection_force_close_pending_read_test,
        connection_cross_thread_force_close_pending_read_test,
        connection_force_close_pending_read_mixed_timing_soak_test,
        connection_force_close_pending_write_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        tcp_connection_harness_test_text = tcp_connection_harness_test.read_text(encoding="utf-8")
        require(tcp_connection_harness_test_text, '#include "support/TcpConnectionHarness.h"', tcp_connection_harness_test)
        require(tcp_connection_harness_test_text, "gamenet::test::makeTcpConnection(", tcp_connection_harness_test)

    for loop_watchdog_test in (
        server_contract_test,
        server_stop_test,
        server_stop_active_write_test,
        server_stop_soak_test,
        server_stop_multi_worker_test,
        server_stop_worker_active_write_soak_test,
        server_stop_from_worker_callback_soak_test,
        server_repeated_stop_test,
        client_contract_test,
        client_cross_thread_disconnect_active_test,
        client_cross_thread_connect_test,
        client_destroy_active_connection_test,
        client_stop_active_connection_mixed_timing_soak_test,
        client_repeated_stop_test,
        client_repeated_connect_test,
        connection_repeated_shutdown_test,
        connection_repeated_force_close_test,
        connection_cross_thread_force_close_soak_test,
        connection_force_close_pending_read_test,
        connection_cross_thread_force_close_pending_read_test,
        connection_force_close_pending_read_mixed_timing_soak_test,
        connection_force_close_pending_write_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        loop_watchdog_text = loop_watchdog_test.read_text(encoding="utf-8")
        require(loop_watchdog_text, '#include "support/LoopTest.h"', loop_watchdog_test)
        require(loop_watchdog_text, "gamenet::test::runLoopWithTimeout", loop_watchdog_test)

    for client_socket_test in (
        acceptor_contract_test,
        socket_contract_test,
        server_contract_test,
        server_stop_test,
        server_stop_active_write_test,
        server_stop_soak_test,
        server_stop_multi_worker_test,
        server_stop_worker_active_write_soak_test,
        server_stop_from_worker_callback_soak_test,
        server_repeated_stop_test,
        connection_peer_reset_test,
    ):
        client_socket_text = client_socket_test.read_text(encoding="utf-8")
        require(client_socket_text, '#include "support/ClientSocket.h"', client_socket_test)
        require(client_socket_text, "gamenet::test::connectTestClient", client_socket_test)
        assert "SocketFd connectTo(" not in client_socket_text, (
            f"{client_socket_test} must use ClientSocket.h instead of a local connectTo helper"
        )

    for client_socket_cleanup_test in (
        acceptor_contract_test,
        socket_contract_test,
        server_contract_test,
        server_stop_test,
        server_stop_active_write_test,
        server_stop_soak_test,
        server_stop_multi_worker_test,
        server_stop_worker_active_write_soak_test,
        server_stop_from_worker_callback_soak_test,
        server_repeated_stop_test,
    ):
        client_socket_cleanup_text = client_socket_cleanup_test.read_text(encoding="utf-8")
        require(client_socket_cleanup_text, "gamenet::test::closeTestSocket", client_socket_cleanup_test)

    for tcp_server_harness_test in (
        server_stop_multi_worker_test,
        server_stop_worker_active_write_soak_test,
        server_stop_from_worker_callback_soak_test,
    ):
        tcp_server_harness_text = tcp_server_harness_test.read_text(encoding="utf-8")
        require(tcp_server_harness_text, '#include "support/TcpServerHarness.h"', tcp_server_harness_test)
        require(tcp_server_harness_text, "gamenet::test::WorkerLoopTracker workerLoopTracker", tcp_server_harness_test)
        require(tcp_server_harness_text, "workerLoopTracker.recordCurrentThread()", tcp_server_harness_test)
        require(tcp_server_harness_text, "gamenet::test::connectTestClients", tcp_server_harness_test)
        require(tcp_server_harness_text, "workerLoopTracker.requireAtLeast(2)", tcp_server_harness_test)
        assert "workerLoopIds" not in tcp_server_harness_text, (
            f"{tcp_server_harness_test} must use TcpServerHarness.h for worker-loop tracking"
        )
        assert "std::mutex" not in tcp_server_harness_text, (
            f"{tcp_server_harness_test} must use TcpServerHarness.h instead of local mutex tracking"
        )
        assert "std::set" not in tcp_server_harness_text, (
            f"{tcp_server_harness_test} must use TcpServerHarness.h instead of local set tracking"
        )

    for thread_handoff_test in (
        client_cross_thread_stop_pending_connect_test,
        client_cross_thread_disconnect_active_test,
        client_repeated_disconnect_test,
        client_repeated_stop_test,
        client_repeated_connect_test,
        client_cross_thread_connect_test,
        client_cross_thread_retry_config_test,
        connection_cross_thread_send_test,
        connection_send_after_close_test,
        connection_cross_thread_shutdown_test,
        connection_repeated_shutdown_test,
        connection_cross_thread_force_close_soak_test,
        connection_cross_thread_force_close_pending_read_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        thread_handoff_text = thread_handoff_test.read_text(encoding="utf-8")
        require(thread_handoff_text, '#include "support/ThreadHandoff.h"', thread_handoff_test)
        require(thread_handoff_text, "gamenet::test::runFromNonOwnerThread", thread_handoff_test)

    for mixed_timing_handoff_test in (
        client_stop_pending_connect_mixed_timing_soak_test,
        client_stop_active_connection_mixed_timing_soak_test,
        connection_force_close_pending_read_mixed_timing_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
    ):
        mixed_timing_handoff_text = mixed_timing_handoff_test.read_text(encoding="utf-8")
        require(mixed_timing_handoff_text, '#include "support/ThreadHandoff.h"', mixed_timing_handoff_test)
        require(mixed_timing_handoff_text, "gamenet::test::NonOwnerThreadHandoff", mixed_timing_handoff_test)
        require(mixed_timing_handoff_text, "gamenet::test::startNonOwnerThread(", mixed_timing_handoff_test)
        require(mixed_timing_handoff_text, "gamenet::test::startNonOwnerThreadAfter(", mixed_timing_handoff_test)
        assert "std::this_thread::sleep_for" not in mixed_timing_handoff_text, (
            f"{mixed_timing_handoff_test} must use ThreadHandoff.h for delayed non-owner timing"
        )

    for small_buffer_socketpair_test in (
        connection_shutdown_pending_test,
        connection_cross_thread_shutdown_test,
        connection_repeated_shutdown_test,
        connection_high_water_test,
        connection_force_close_pending_write_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        small_buffer_socketpair_text = small_buffer_socketpair_test.read_text(encoding="utf-8")
        require(small_buffer_socketpair_text, '#include "support/SocketPair.h"', small_buffer_socketpair_test)
        require(small_buffer_socketpair_text, "gamenet::test::ConnectedSocketPair pair(", small_buffer_socketpair_test)
        require(
            small_buffer_socketpair_text,
            "gamenet::test::SocketPairMode::SmallSendBuffer",
            small_buffer_socketpair_test,
        )

    tcp_client_intent_text = tcp_client_intent.read_text(encoding="utf-8")
    require(
        tcp_client_intent_text,
        "enableRetry() and disableRetry() may be called cross-thread",
        tcp_client_intent,
    )
    require(tcp_client_intent_text, "stop() cancels pending retry", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_retry_stop_race.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_retry_stop_soak.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_stop_pending_connect.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_stop_pending_connect_soak.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_cross_thread_stop_pending_connect.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_stop_pending_connect_mixed_timing_soak.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_destroy_pending_connect.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_destroy_active_connection.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_stop_active_connection_mixed_timing_soak.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_cross_thread_disconnect_active.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_repeated_disconnect.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_repeated_stop.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_repeated_connect.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_cross_thread_connect.cpp", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_cross_thread_retry_config.cpp", tcp_client_intent)

    connector_intent = repo_root / "intents" / "modules" / "connector.intent.md"
    connector_intent_text = connector_intent.read_text(encoding="utf-8")
    require(connector_intent_text, "stop() during pending retry cancels timer", connector_intent)
    require(connector_intent_text, "test_connector_retry_stop.cpp", connector_intent)

    tcp_server_intent_text = tcp_server_intent.read_text(encoding="utf-8")
    require(tcp_server_intent_text, "stop() during active write", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_active_write.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_soak.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_multi_worker.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_worker_active_write_soak.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_from_worker_callback_soak.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_repeated_stop.cpp", tcp_server_intent)

    tcp_connection_intent_text = tcp_connection_intent.read_text(encoding="utf-8")
    require(tcp_connection_intent_text, "peer close or reset converges on the normal close path", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_peer_close.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_peer_reset.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "error-triggered teardown remains single-shot", tcp_connection_intent)
    require(tcp_connection_intent_text, "write-complete callback is queued after send returns", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_write_complete_ordering.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_cross_thread_send.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_send_after_close.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "shutdown waits for pending output", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_shutdown_pending_output.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_cross_thread_shutdown.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_repeated_shutdown.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "high-water callback fires once when output crosses the threshold", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_high_water_mark.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "repeated forceClose calls are idempotent", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_repeated_force_close.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "repeated teardown does not leave stale registration behind", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_repeated_connect_destroyed.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_cross_thread_force_close_soak.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "pending IOCP read/write operations are canceled", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_force_close_pending_read.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_cross_thread_force_close_pending_read.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_force_close_pending_read_mixed_timing_soak.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_force_close_pending_write_soak.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_force_close_pending_write_mixed_timing_soak.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_cross_thread_force_close_pending_write.cpp", tcp_connection_intent)

    iocp_transport_header_text = iocp_transport_header.read_text(encoding="utf-8")
    require(iocp_transport_header_text, "cancelPendingOperations", iocp_transport_header)
    require(iocp_transport_header_text, "hasPendingOperations", iocp_transport_header)

    iocp_transport_source_text = iocp_transport_source.read_text(encoding="utf-8")
    require(iocp_transport_source_text, "CancelIoEx", iocp_transport_source)
    require(iocp_transport_source_text, "ERROR_NOT_FOUND", iocp_transport_source)

    tcp_connection_source_text = tcp_connection_source.read_text(encoding="utf-8")
    require(tcp_connection_source_text, "cancelPendingOperations", tcp_connection_source)
    require(tcp_connection_source_text, "finishClose", tcp_connection_source)
    require(tcp_connection_source_text, "forceClosePending_", tcp_connection_source)

    tcp_connection_header_text = tcp_connection_header.read_text(encoding="utf-8")
    require(tcp_connection_header_text, "forceCloseGuard_", tcp_connection_header)

    tcp_client_source_text = tcp_client_source.read_text(encoding="utf-8")
    require(tcp_client_source_text, "connection->connectDestroyed();", tcp_client_source)
    require(tcp_client_source_text, "if (!conn->disconnected())", tcp_client_source)
    require(tcp_client_source_text, "conn->forceClose();", tcp_client_source)
    require(tcp_client_source_text, "void TcpClient::setRetryInLoop(bool enabled) noexcept", tcp_client_source)
    require(tcp_client_source_text, "loop_->runInLoop([this, lifetime, enabled]", tcp_client_source)
    require(tcp_client_source_text, "connector_->setRetryEnabled(enabled);", tcp_client_source)

    tcp_client_header_text = tcp_client_header.read_text(encoding="utf-8")
    require(tcp_client_header_text, "void setRetryInLoop(bool enabled) noexcept;", tcp_client_header)

    tcp_server_source_text = tcp_server_source.read_text(encoding="utf-8")
    require(tcp_server_source_text, "bool TcpServer::forceCloseAllConnections()", tcp_server_source)
    require(tcp_server_source_text, "notifyClosed", tcp_server_source)
    require(tcp_server_source_text, "setCloseCallback([notifyClosed](const TcpConnectionPtr& conn)", tcp_server_source)
    require(tcp_server_source_text, "conn->connectDestroyed();", tcp_server_source)

    connector_header_text = connector_header.read_text(encoding="utf-8")
    require(connector_header_text, "connectStopGuard_", connector_header)
    require(connector_header_text, "finishCancelInLoop", connector_header)

    connector_source_text = connector_source.read_text(encoding="utf-8")
    require(connector_source_text, "CancelIoEx", connector_source)
    require(connector_source_text, "finishCancelInLoop", connector_source)
    require(connector_source_text, "ERROR_NOT_FOUND", connector_source)
    require(connector_source_text, "SocketFd Connector::removeAndReleaseChannel()", connector_source)
    require(connector_source_text, "std::shared_ptr<Channel>(std::move(channel_))", connector_source)
    require(connector_source_text, "loop_->queueInLoop([deferredChannel]", connector_source)
    assert "resetChannel" not in connector_source_text, (
        "Connector must vacate channel_ synchronously instead of leaving the member occupied until a queued reset"
    )

    iocp_poller_source_text = iocp_poller_source.read_text(encoding="utf-8")
    require(iocp_poller_source_text, "IocpOperationKind::Read", iocp_poller_source)
    require(iocp_poller_source_text, "IocpOperationKind::Write", iocp_poller_source)
    require(iocp_poller_source_text, "return Channel::kReadEvent", iocp_poller_source)
    require(iocp_poller_source_text, "return Channel::kWriteEvent", iocp_poller_source)

    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    require(tests_cmake_text, "test_tcp_server_stop_active_connections.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_active_write.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_multi_worker.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_multi_worker.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_worker_active_write_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_worker_active_write_soak.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_from_worker_callback_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_stop_from_worker_callback_soak.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_repeated_stop.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_server_repeated_stop.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_retry_stop_race.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_retry_stop_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_stop_pending_connect.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_stop_pending_connect_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_stop_pending_connect.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_stop_pending_connect.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_stop_pending_connect_mixed_timing_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_stop_pending_connect_mixed_timing_soak.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_destroy_pending_connect.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_destroy_pending_connect.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_destroy_active_connection.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_destroy_active_connection.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_stop_active_connection_mixed_timing_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_stop_active_connection_mixed_timing_soak.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_disconnect_active.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_disconnect_active.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_repeated_disconnect.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_repeated_disconnect.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_repeated_stop.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_repeated_stop.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_repeated_connect.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_repeated_connect.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_connect.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_connect.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_retry_config.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_retry_config.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_connector_retry_stop.cpp", tests_cmake)
    require(tests_cmake_text, "test_connector_retry_stop.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_peer_close.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_peer_reset.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_write_complete_ordering.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_send.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_send_after_close.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_send_after_close.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_shutdown_pending_output.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_shutdown.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_repeated_shutdown.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_repeated_shutdown.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_high_water_mark.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_repeated_force_close.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_repeated_connect_destroyed.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_repeated_connect_destroyed.cpp lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_force_close_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_read.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_read.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_force_close_pending_read.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_force_close_pending_read.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_read_mixed_timing_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_read_mixed_timing_soak.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_write_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_write_soak.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_write_mixed_timing_soak.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_force_close_pending_write_mixed_timing_soak.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_force_close_pending_write.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_force_close_pending_write.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "contract tcp_server", tests_cmake)
    require(tests_cmake_text, "contract tcp_client", tests_cmake)
    require(tests_cmake_text, "contract tcp_connection", tests_cmake)
    require(tests_cmake_text, "threading lifecycle", tests_cmake)

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_tcp_lifecycle_contracts.py", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_tcp_lifecycle_contracts.py", ci_contract)


if __name__ == "__main__":
    main()
