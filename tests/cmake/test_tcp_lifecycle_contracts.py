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
    client_cross_thread_disconnect_active_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_cross_thread_disconnect_active.cpp"
    )
    client_cross_thread_connect_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_client"
        / "test_tcp_client_cross_thread_connect.cpp"
    )
    connection_peer_close_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_peer_close.cpp"
    )
    connection_peer_reset_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_peer_reset.cpp"
    )
    connection_write_complete_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_write_complete_ordering.cpp"
    )
    connection_cross_thread_send_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_cross_thread_send.cpp"
    )
    connection_shutdown_pending_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_shutdown_pending_output.cpp"
    )
    connection_cross_thread_shutdown_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_cross_thread_shutdown.cpp"
    )
    connection_high_water_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_high_water_mark.cpp"
    )
    connection_repeated_force_close_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_repeated_force_close.cpp"
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
    tcp_connection_source = repo_root / "src" / "core" / "net" / "TcpConnection.cc"
    tcp_connection_header = repo_root / "include" / "gamenet" / "core" / "net" / "TcpConnection.h"
    tcp_client_source = repo_root / "src" / "core" / "net" / "TcpClient.cc"
    tcp_server_source = repo_root / "src" / "core" / "net" / "TcpServer.cc"
    connector_header = repo_root / "include" / "gamenet" / "core" / "net" / "Connector.h"
    connector_source = repo_root / "src" / "core" / "net" / "Connector.cc"
    iocp_poller_source = repo_root / "src" / "core" / "net" / "poller" / "IocpPoller.cc"

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
    assert client_cross_thread_disconnect_active_test.exists(), (
        "missing TCP client active cross-thread disconnect contract: "
        f"{client_cross_thread_disconnect_active_test}"
    )
    assert client_cross_thread_connect_test.exists(), (
        f"missing TCP client cross-thread connect contract: {client_cross_thread_connect_test}"
    )
    assert connection_peer_close_test.exists(), (
        f"missing TCP connection peer-close contract: {connection_peer_close_test}"
    )
    assert connection_peer_reset_test.exists(), (
        f"missing TCP connection peer-reset contract: {connection_peer_reset_test}"
    )
    assert connection_write_complete_test.exists(), (
        f"missing TCP connection write-complete ordering contract: {connection_write_complete_test}"
    )
    assert connection_cross_thread_send_test.exists(), (
        f"missing TCP connection cross-thread send contract: {connection_cross_thread_send_test}"
    )
    assert connection_shutdown_pending_test.exists(), (
        f"missing TCP connection shutdown-pending-output contract: {connection_shutdown_pending_test}"
    )
    assert connection_cross_thread_shutdown_test.exists(), (
        f"missing TCP connection cross-thread shutdown contract: {connection_cross_thread_shutdown_test}"
    )
    assert connection_high_water_test.exists(), (
        f"missing TCP connection high-water mark contract: {connection_high_water_test}"
    )
    assert connection_repeated_force_close_test.exists(), (
        f"missing TCP connection repeated force-close contract: {connection_repeated_force_close_test}"
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
    assert connection_lifecycle_test.exists(), f"missing TCP connection lifecycle contract: {connection_lifecycle_test}"

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
    require(server_stop_multi_worker_text, "GAMENET_TEST_ASSERT(workerLoopIds.size() >= 2)", server_stop_multi_worker_test)
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
        "GAMENET_TEST_ASSERT(workerLoopIds.size() >= 2)",
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
        "GAMENET_TEST_ASSERT(workerLoopIds.size() >= 2)",
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

    client_test_text = client_retry_stop_test.read_text(encoding="utf-8")
    require(client_test_text, "client-retry-stop-race", client_retry_stop_test)
    require(client_test_text, "client.enableRetry();", client_retry_stop_test)
    require(client_test_text, "client.stop();", client_retry_stop_test)
    require(client_test_text, "server.start();", client_retry_stop_test)
    require(client_test_text, "GAMENET_TEST_ASSERT(!connectedAfterStop)", client_retry_stop_test)
    require(client_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", client_retry_stop_test)

    client_retry_stop_soak_text = client_retry_stop_soak_test.read_text(encoding="utf-8")
    require(client_retry_stop_soak_text, "client-retry-stop-soak", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "constexpr int iterationCount", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "client.enableRetry();", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "client.stop();", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "server.start();", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "GAMENET_TEST_ASSERT(!connectedAfterStop)", client_retry_stop_soak_test)
    require(client_retry_stop_soak_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", client_retry_stop_soak_test)

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
    require(client_cross_thread_stop_pending_connect_text, "std::thread stopper", client_cross_thread_stop_pending_connect_test)
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
        "std::thread stopper",
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
        "std::thread stopper",
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

    client_cross_thread_disconnect_active_text = client_cross_thread_disconnect_active_test.read_text(encoding="utf-8")
    require(
        client_cross_thread_disconnect_active_text,
        "client-cross-thread-disconnect-active",
        client_cross_thread_disconnect_active_test,
    )
    require(
        client_cross_thread_disconnect_active_text,
        "std::thread disconnectThread",
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

    client_cross_thread_connect_text = client_cross_thread_connect_test.read_text(encoding="utf-8")
    require(client_cross_thread_connect_text, "client-cross-thread-connect", client_cross_thread_connect_test)
    require(client_cross_thread_connect_text, "std::thread connectThread", client_cross_thread_connect_test)
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
    require(cross_thread_send_test_text, "std::thread sendThread", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "conn->send(payload);", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "GAMENET_TEST_ASSERT(peerReadBytes == payload.size())", connection_cross_thread_send_test)
    require(cross_thread_send_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_cross_thread_send_test)

    shutdown_pending_test_text = connection_shutdown_pending_test.read_text(encoding="utf-8")
    require(shutdown_pending_test_text, "shutdown-waits-for-pending-output", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "conn->send(payload);", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "conn->shutdown();", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(peerSawEof)", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_shutdown_pending_test)

    cross_thread_shutdown_test_text = connection_cross_thread_shutdown_test.read_text(encoding="utf-8")
    require(cross_thread_shutdown_test_text, "cross-thread-shutdown-drains-output", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "std::thread shutdownThread", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "conn->shutdown();", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(peerSawEof)", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(closeCallbackCount == 1)", connection_cross_thread_shutdown_test)
    require(cross_thread_shutdown_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_cross_thread_shutdown_test)

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
    require(repeated_force_close_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)", connection_repeated_force_close_test)
    require(repeated_force_close_text, "GAMENET_TEST_ASSERT(closeCallbackCount == 1)", connection_repeated_force_close_test)
    require(repeated_force_close_text, "conn->connectDestroyed();", connection_repeated_force_close_test)

    cross_thread_force_close_soak_text = connection_cross_thread_force_close_soak_test.read_text(encoding="utf-8")
    require(
        cross_thread_force_close_soak_text,
        "cross-thread-force-close-soak-contract",
        connection_cross_thread_force_close_soak_test,
    )
    require(cross_thread_force_close_soak_text, "constexpr int iterationCount", connection_cross_thread_force_close_soak_test)
    require(cross_thread_force_close_soak_text, "std::thread closer", connection_cross_thread_force_close_soak_test)
    require(cross_thread_force_close_soak_text, "conn->forceClose();", connection_cross_thread_force_close_soak_test)
    require(
        cross_thread_force_close_soak_text,
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)",
        connection_cross_thread_force_close_soak_test,
    )
    require(
        cross_thread_force_close_soak_text,
        "GAMENET_TEST_ASSERT(closeCallbackCount == 1)",
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
    require(force_close_pending_read_text, "GAMENET_TEST_ASSERT(closeCallbackCount == 1)", connection_force_close_pending_read_test)
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
    require(cross_thread_force_close_pending_read_text, "std::thread closer", connection_cross_thread_force_close_pending_read_test)
    require(cross_thread_force_close_pending_read_text, "conn->connectEstablished();", connection_cross_thread_force_close_pending_read_test)
    require(cross_thread_force_close_pending_read_text, "conn->forceClose();", connection_cross_thread_force_close_pending_read_test)
    require(cross_thread_force_close_pending_read_text, "conn->connectDestroyed();", connection_cross_thread_force_close_pending_read_test)
    require(cross_thread_force_close_pending_read_text, "connection.reset();", connection_cross_thread_force_close_pending_read_test)
    require(
        cross_thread_force_close_pending_read_text,
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)",
        connection_cross_thread_force_close_pending_read_test,
    )
    require(
        cross_thread_force_close_pending_read_text,
        "GAMENET_TEST_ASSERT(closeCallbackCount == 1)",
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
        "std::thread closer",
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
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)",
        connection_force_close_pending_read_mixed_timing_soak_test,
    )
    require(
        force_close_pending_read_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(closeCallbackCount == 1)",
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
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)",
        connection_force_close_pending_write_soak_test,
    )
    require(
        force_close_pending_write_soak_text,
        "GAMENET_TEST_ASSERT(closeCallbackCount == 1)",
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
        "std::thread closer",
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
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)",
        connection_force_close_pending_write_mixed_timing_soak_test,
    )
    require(
        force_close_pending_write_mixed_timing_soak_text,
        "GAMENET_TEST_ASSERT(closeCallbackCount == 1)",
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
    require(cross_thread_force_close_pending_write_text, "std::thread closer", connection_cross_thread_force_close_pending_write_test)
    require(cross_thread_force_close_pending_write_text, "conn->send(payload);", connection_cross_thread_force_close_pending_write_test)
    require(cross_thread_force_close_pending_write_text, "conn->forceClose();", connection_cross_thread_force_close_pending_write_test)
    require(
        cross_thread_force_close_pending_write_text,
        "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)",
        connection_cross_thread_force_close_pending_write_test,
    )
    require(
        cross_thread_force_close_pending_write_text,
        "GAMENET_TEST_ASSERT(closeCallbackCount == 1)",
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
        connection_high_water_test,
        connection_repeated_force_close_test,
        connection_cross_thread_force_close_soak_test,
        connection_force_close_pending_read_test,
        connection_cross_thread_force_close_pending_read_test,
        connection_force_close_pending_read_mixed_timing_soak_test,
        connection_force_close_pending_write_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        socketpair_text = socketpair_test.read_text(encoding="utf-8")
        require(socketpair_text, '#include "gamenet/core/net/SocketTypes.h"', socketpair_test)

    for pending_write_socketpair_test in (
        connection_force_close_pending_write_soak_test,
        connection_force_close_pending_write_mixed_timing_soak_test,
        connection_cross_thread_force_close_pending_write_test,
    ):
        pending_write_socketpair_text = pending_write_socketpair_test.read_text(encoding="utf-8")
        require(pending_write_socketpair_text, "setNonBlockingForTest(connectionFd);", pending_write_socketpair_test)
        require(pending_write_socketpair_text, "setNonBlockingForTest(peerFd);", pending_write_socketpair_test)
        require(pending_write_socketpair_text, "setSmallSendBuffer(connectionFd);", pending_write_socketpair_test)
        require(
            pending_write_socketpair_text,
            "SO_SNDBUF,\n        reinterpret_cast<const char*>(&bufferSize)",
            pending_write_socketpair_test,
        )
        require(pending_write_socketpair_text, "SO_SNDBUF,\n        &bufferSize", pending_write_socketpair_test)

    tcp_client_intent_text = tcp_client_intent.read_text(encoding="utf-8")
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
    require(tcp_client_intent_text, "test_tcp_client_cross_thread_connect.cpp", tcp_client_intent)

    tcp_server_intent_text = tcp_server_intent.read_text(encoding="utf-8")
    require(tcp_server_intent_text, "stop() during active write", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_active_write.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_soak.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_multi_worker.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_worker_active_write_soak.cpp", tcp_server_intent)
    require(tcp_server_intent_text, "test_tcp_server_stop_from_worker_callback_soak.cpp", tcp_server_intent)

    tcp_connection_intent_text = tcp_connection_intent.read_text(encoding="utf-8")
    require(tcp_connection_intent_text, "peer close or reset converges on the normal close path", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_peer_close.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_peer_reset.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "error-triggered teardown remains single-shot", tcp_connection_intent)
    require(tcp_connection_intent_text, "write-complete callback is queued after send returns", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_write_complete_ordering.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_cross_thread_send.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "shutdown waits for pending output", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_shutdown_pending_output.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_cross_thread_shutdown.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "high-water callback fires once when output crosses the threshold", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_high_water_mark.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "repeated forceClose calls are idempotent", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_repeated_force_close.cpp", tcp_connection_intent)
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
    require(tests_cmake_text, "test_tcp_client_cross_thread_connect.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_cross_thread_connect.cpp threading lifecycle", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_peer_close.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_peer_reset.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_write_complete_ordering.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_send.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_shutdown_pending_output.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_cross_thread_shutdown.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_high_water_mark.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_repeated_force_close.cpp", tests_cmake)
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
