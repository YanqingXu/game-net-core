from __future__ import annotations

import re
from pathlib import Path


def require(condition: bool, message: str) -> None:
    assert condition, message


def function_body(text: str, signature: str, source: Path) -> str:
    match = re.search(r"(?m)^" + re.escape(signature) + r"\s*\{", text)
    require(match is not None, f"missing function in {source}: {signature}")
    brace_start = match.end() - 1

    depth = 0
    for index in range(brace_start, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace_start + 1 : index]

    raise AssertionError(f"unterminated function body in {source}: {signature}")


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    header = repo_root / "include" / "gamenet" / "core" / "net" / "TcpConnection.h"
    source = repo_root / "src" / "core" / "net" / "TcpConnection.cc"
    server_source = repo_root / "src" / "core" / "net" / "TcpServer.cc"
    contract_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_cross_thread_state.cpp"
    )
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    long_soak_workflow = repo_root / ".github" / "workflows" / "long-soak.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    header_text = header.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    server_source_text = server_source.read_text(encoding="utf-8")
    contract_text = contract_test.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    long_soak_workflow_text = long_soak_workflow.read_text(encoding="utf-8")
    ci_contract_text = ci_contract.read_text(encoding="utf-8")

    require(
        "std::atomic<StateE> state_" in header_text,
        "TcpConnection public state must use atomic storage for cross-thread observation",
    )
    require(
        "cross-thread-safe state snapshots" in header_text,
        "TcpConnection.h must document connected/disconnected thread semantics",
    )

    for signature in (
        "bool TcpConnection::connected() const noexcept",
        "bool TcpConnection::disconnected() const noexcept",
    ):
        body = function_body(source_text, signature, source)
        require("state_.load(" in body, f"{signature} must load the atomic state explicitly")

    state_setter = function_body(source_text, "void TcpConnection::setState(StateE state) noexcept", source)
    require("state_.store(" in state_setter, "setState must store the atomic state explicitly")

    for signature in (
        "void TcpConnection::setTcpNoDelay(bool on)",
        "void TcpConnection::setConnectionCallback(ConnectionCallback cb)",
        "void TcpConnection::setMessageCallback(MessageCallback cb)",
        "void TcpConnection::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark)",
        "void TcpConnection::setWriteCompleteCallback(WriteCompleteCallback cb)",
        "void TcpConnection::setCloseCallback(CloseCallback cb)",
    ):
        body = function_body(source_text, signature, source)
        require(
            "loop_->assertInLoopThread();" in body,
            f"{signature} must assert its owner-loop-only contract",
        )

    force_close_all = function_body(
        server_source_text,
        "bool TcpServer::forceCloseAllConnections()",
        server_source,
    )
    run_in_loop = "connectionLoop->runInLoop([connection, notifyClosed]"
    require(run_in_loop in force_close_all, "TcpServer teardown must marshal to each connection loop")
    callback_position = force_close_all.find("connection->setCloseCallback", force_close_all.find(run_in_loop))
    require(callback_position >= 0, "TcpServer must replace the close callback inside the connection-loop task")

    require(
        "connection->connected()" in contract_text and "connection->disconnected()" in contract_text,
        "cross-thread contract test must observe both public connection states",
    )
    require(
        "test_tcp_connection_cross_thread_state.cpp threading lifecycle" in tests_cmake_text,
        "cross-thread state contract must be in the threading and lifecycle CTest slices",
    )
    require(
        "python3 tests/cmake/test_tcp_connection_thread_contract.py" in workflow_text,
        "Linux CI jobs must run the TcpConnection thread-contract guard",
    )
    require(
        "python tests/cmake/test_tcp_connection_thread_contract.py" in workflow_text,
        "Windows CI must run the TcpConnection thread-contract guard",
    )
    require(
        "python3 tests/cmake/test_tcp_connection_thread_contract.py" in long_soak_workflow_text,
        "long-soak must run the TcpConnection thread-contract guard",
    )
    require(
        "python3 tests/cmake/test_tcp_connection_thread_contract.py" in ci_contract_text,
        "workflow contract must require the TcpConnection thread-contract guard",
    )


if __name__ == "__main__":
    main()
