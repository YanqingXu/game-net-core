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
    tcp_connection_intent = repo_root / "intents" / "modules" / "tcp_connection.intent.md"
    server_stop_test = (
        repo_root / "tests" / "contract" / "tcp_server" / "test_tcp_server_stop_active_connections.cpp"
    )
    client_retry_stop_test = (
        repo_root / "tests" / "contract" / "tcp_client" / "test_tcp_client_retry_stop_race.cpp"
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
    connection_shutdown_pending_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_shutdown_pending_output.cpp"
    )
    connection_high_water_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_high_water_mark.cpp"
    )
    connection_repeated_force_close_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_repeated_force_close.cpp"
    )
    connection_lifecycle_test = (
        repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_lifecycle.cpp"
    )

    assert server_stop_test.exists(), f"missing TCP server stop active-connection contract: {server_stop_test}"
    assert client_retry_stop_test.exists(), f"missing TCP client retry-stop race contract: {client_retry_stop_test}"
    assert connection_peer_close_test.exists(), (
        f"missing TCP connection peer-close contract: {connection_peer_close_test}"
    )
    assert connection_peer_reset_test.exists(), (
        f"missing TCP connection peer-reset contract: {connection_peer_reset_test}"
    )
    assert connection_write_complete_test.exists(), (
        f"missing TCP connection write-complete ordering contract: {connection_write_complete_test}"
    )
    assert connection_shutdown_pending_test.exists(), (
        f"missing TCP connection shutdown-pending-output contract: {connection_shutdown_pending_test}"
    )
    assert connection_high_water_test.exists(), (
        f"missing TCP connection high-water mark contract: {connection_high_water_test}"
    )
    assert connection_repeated_force_close_test.exists(), (
        f"missing TCP connection repeated force-close contract: {connection_repeated_force_close_test}"
    )
    assert connection_lifecycle_test.exists(), f"missing TCP connection lifecycle contract: {connection_lifecycle_test}"

    test_text = server_stop_test.read_text(encoding="utf-8")
    require(test_text, "stop-reentrant-disconnect", server_stop_test)
    require(test_text, "server.stop();", server_stop_test)
    require(test_text, "server.connectionCount() == 0", server_stop_test)
    require(test_text, "GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1)", server_stop_test)
    require(test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", server_stop_test)

    client_test_text = client_retry_stop_test.read_text(encoding="utf-8")
    require(client_test_text, "client-retry-stop-race", client_retry_stop_test)
    require(client_test_text, "client.enableRetry();", client_retry_stop_test)
    require(client_test_text, "client.stop();", client_retry_stop_test)
    require(client_test_text, "server.start();", client_retry_stop_test)
    require(client_test_text, "GAMENET_TEST_ASSERT(!connectedAfterStop)", client_retry_stop_test)
    require(client_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", client_retry_stop_test)

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

    shutdown_pending_test_text = connection_shutdown_pending_test.read_text(encoding="utf-8")
    require(shutdown_pending_test_text, "shutdown-waits-for-pending-output", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "conn->send(payload);", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "conn->shutdown();", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1)", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(peerSawEof)", connection_shutdown_pending_test)
    require(shutdown_pending_test_text, "GAMENET_TEST_ASSERT(loop.isInLoopThread())", connection_shutdown_pending_test)

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

    for socketpair_test in (
        connection_lifecycle_test,
        connection_peer_close_test,
        connection_peer_reset_test,
        connection_write_complete_test,
        connection_shutdown_pending_test,
        connection_high_water_test,
        connection_repeated_force_close_test,
    ):
        socketpair_text = socketpair_test.read_text(encoding="utf-8")
        require(socketpair_text, '#include "gamenet/core/net/SocketTypes.h"', socketpair_test)

    tcp_client_intent_text = tcp_client_intent.read_text(encoding="utf-8")
    require(tcp_client_intent_text, "stop() cancels pending retry", tcp_client_intent)
    require(tcp_client_intent_text, "test_tcp_client_retry_stop_race.cpp", tcp_client_intent)

    tcp_connection_intent_text = tcp_connection_intent.read_text(encoding="utf-8")
    require(tcp_connection_intent_text, "peer close or reset converges on the normal close path", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_peer_close.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_peer_reset.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "error-triggered teardown remains single-shot", tcp_connection_intent)
    require(tcp_connection_intent_text, "write-complete callback is queued after send returns", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_write_complete_ordering.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "shutdown waits for pending output", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_shutdown_pending_output.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "high-water callback fires once when output crosses the threshold", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_high_water_mark.cpp", tcp_connection_intent)
    require(tcp_connection_intent_text, "repeated forceClose calls are idempotent", tcp_connection_intent)
    require(tcp_connection_intent_text, "test_tcp_connection_repeated_force_close.cpp", tcp_connection_intent)

    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    require(tests_cmake_text, "test_tcp_server_stop_active_connections.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_client_retry_stop_race.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_peer_close.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_peer_reset.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_write_complete_ordering.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_shutdown_pending_output.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_high_water_mark.cpp", tests_cmake)
    require(tests_cmake_text, "test_tcp_connection_repeated_force_close.cpp", tests_cmake)
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
