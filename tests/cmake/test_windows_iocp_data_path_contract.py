from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing IOCP data-path fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    spec = repo_root / "docs" / "superpowers" / "specs" / "2026-07-07-windows-iocp-data-path-design.md"
    plan = repo_root / "docs" / "superpowers" / "plans" / "2026-07-07-windows-iocp-data-path.md"
    milestone = repo_root / "docs" / "development" / "windows_iocp_milestone.md"
    operation = repo_root / "include" / "gamenet" / "core" / "net" / "platform" / "IocpOperation.h"
    socket_ops = repo_root / "include" / "gamenet" / "core" / "net" / "platform" / "IocpSocketOps.h"
    socket_ops_source = repo_root / "src" / "core" / "net" / "platform" / "IocpSocketOps_win.cc"
    sockets_win = repo_root / "src" / "core" / "net" / "platform" / "SocketsOps_win.cc"
    acceptor_header = repo_root / "include" / "gamenet" / "core" / "net" / "Acceptor.h"
    acceptor_source = repo_root / "src" / "core" / "net" / "Acceptor.cc"
    connector_header = repo_root / "include" / "gamenet" / "core" / "net" / "Connector.h"
    connector_source = repo_root / "src" / "core" / "net" / "Connector.cc"
    tcp_connection_header = repo_root / "include" / "gamenet" / "core" / "net" / "TcpConnection.h"
    tcp_connection_source = repo_root / "src" / "core" / "net" / "TcpConnection.cc"
    tcp_transport = repo_root / "src" / "core" / "net" / "platform" / "IocpTcpTransport.h"
    tcp_transport_source = repo_root / "src" / "core" / "net" / "platform" / "IocpTcpTransport_win.cc"
    poller_header = repo_root / "include" / "gamenet" / "core" / "net" / "poller" / "IocpPoller.h"
    poller_source = repo_root / "src" / "core" / "net" / "poller" / "IocpPoller.cc"
    core_cmake = repo_root / "src" / "core" / "CMakeLists.txt"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    require(spec.read_text(encoding="utf-8"), "loop-owned Windows IOCP operation layer", spec)
    require(plan.read_text(encoding="utf-8"), "WSARecv", plan)
    require(milestone.read_text(encoding="utf-8"), "windows-iocp-data-path-design.md", milestone)

    operation_text = operation.read_text(encoding="utf-8")
    require(operation_text, "enum class IocpOperationKind", operation)
    require(operation_text, "OVERLAPPED overlapped", operation)
    require(operation_text, "bytesTransferred", operation)
    require(operation_text, "Channel* channel", operation)

    socket_ops_text = socket_ops.read_text(encoding="utf-8")
    require(socket_ops_text, "loadAcceptEx", socket_ops)
    require(socket_ops_text, "loadConnectEx", socket_ops)
    require(socket_ops_text, "SocketFd createOverlappedTcp(sa_family_t family);", socket_ops)
    require(socket_ops_text, "bool bindUnspecified", socket_ops)
    require(socket_ops_text, "bool updateAcceptContext", socket_ops)
    require(socket_ops_text, "bool updateConnectContext", socket_ops)

    socket_ops_source_text = socket_ops_source.read_text(encoding="utf-8")
    require(socket_ops_source_text, "WSASocketW", socket_ops_source)
    require(socket_ops_source_text, "WSA_FLAG_OVERLAPPED", socket_ops_source)
    require(socket_ops_source_text, "SIO_GET_EXTENSION_FUNCTION_POINTER", socket_ops_source)

    sockets_win_text = sockets_win.read_text(encoding="utf-8")
    require(sockets_win_text, "SocketFd createNonblocking(sa_family_t family)", sockets_win)

    acceptor_header_text = acceptor_header.read_text(encoding="utf-8")
    require(acceptor_header_text, "IocpAcceptState", acceptor_header)
    require(acceptor_header_text, "postAccept", acceptor_header)

    acceptor_source_text = acceptor_source.read_text(encoding="utf-8")
    require(acceptor_source_text, "platform::loadAcceptEx", acceptor_source)
    require(acceptor_source_text, "platform::createOverlappedTcp", acceptor_source)
    require(acceptor_source_text, "platform::updateAcceptContext", acceptor_source)
    assert "platform::createOverlappedTcpOrDie" not in acceptor_source_text
    assert "platform::updateAcceptContextOrDie" not in acceptor_source_text
    require(acceptor_source_text, "IocpOperationKind::Accept", acceptor_source)
    require(acceptor_source_text, "retainCompletionOperation", acceptor_source)

    connector_header_text = connector_header.read_text(encoding="utf-8")
    require(connector_header_text, "IocpConnectState", connector_header)
    require(connector_header_text, "std::shared_ptr<IocpConnectState>", connector_header)

    connector_source_text = connector_source.read_text(encoding="utf-8")
    require(connector_source_text, "platform::loadConnectEx", connector_source)
    require(connector_source_text, "platform::bindUnspecified", connector_source)
    require(connector_source_text, "platform::updateConnectContext", connector_source)
    assert "platform::bindUnspecifiedOrDie" not in connector_source_text
    assert "platform::updateConnectContextOrDie" not in connector_source_text
    require(connector_source_text, "IocpOperationKind::Connect", connector_source)
    require(connector_source_text, "preserveSocketAssociation", connector_source)
    require(connector_source_text, "retainCompletionOperation", connector_source)
    require(connector_source_text, "retryAfterCancel", connector_source)
    require(connector_source_text, "ERROR_NOT_FOUND", connector_source)

    tcp_connection_header_text = tcp_connection_header.read_text(encoding="utf-8")
    require(tcp_connection_header_text, "IocpTcpTransport", tcp_connection_header)

    tcp_connection_source_text = tcp_connection_source.read_text(encoding="utf-8")
    require(tcp_connection_source_text, "iocpTransport_->startRead", tcp_connection_source)
    require(tcp_connection_source_text, "iocpTransport_->startWrite", tcp_connection_source)
    require(tcp_connection_source_text, "iocpTransport_->completeRead", tcp_connection_source)
    require(tcp_connection_source_text, "iocpTransport_->completeWrite", tcp_connection_source)

    tcp_transport_text = tcp_transport.read_text(encoding="utf-8")
    require(tcp_transport_text, "class IocpTcpTransport", tcp_transport)
    require(tcp_transport_text, "startRead", tcp_transport)
    require(tcp_transport_text, "completeRead", tcp_transport)
    require(tcp_transport_text, "startWrite", tcp_transport)
    require(tcp_transport_text, "completeWrite", tcp_transport)
    require(tcp_transport_text, "writeStorage_", tcp_transport)

    tcp_transport_source_text = tcp_transport_source.read_text(encoding="utf-8")
    require(tcp_transport_source_text, "WSARecv", tcp_transport_source)
    require(tcp_transport_source_text, "WSASend", tcp_transport_source)
    require(tcp_transport_source_text, "IocpOperationKind::Read", tcp_transport_source)
    require(tcp_transport_source_text, "IocpOperationKind::Write", tcp_transport_source)

    poller_header_text = poller_header.read_text(encoding="utf-8")
    require(poller_header_text, "Windows IOCP backend for EventLoop", poller_header)
    require(poller_header_text, "loop-owned IocpOperation metadata", poller_header)
    assert "skeleton" not in poller_header_text.lower(), (
        "IocpPoller comments must describe the active data path, not the old skeleton milestone"
    )

    poller_text = poller_source.read_text(encoding="utf-8")
    require(poller_text, "reinterpret_cast<IocpOperation*>", poller_source)
    require(poller_text, "operation->bytesTransferred", poller_source)
    require(poller_text, "IocpOperationKind::Read", poller_source)
    require(poller_text, "IocpOperationKind::Write", poller_source)
    require(poller_text, "associatedFds_", poller_source)

    require(core_cmake.read_text(encoding="utf-8"), "net/platform/IocpSocketOps_win.cc", core_cmake)
    require(core_cmake.read_text(encoding="utf-8"), "net/platform/IocpTcpTransport_win.cc", core_cmake)

    guard_command = "python3 tests/cmake/test_windows_iocp_data_path_contract.py"
    require(ci_docs.read_text(encoding="utf-8"), "test_windows_iocp_data_path_contract.py", ci_docs)
    require(workflow.read_text(encoding="utf-8"), guard_command, workflow)
    require(ci_contract.read_text(encoding="utf-8"), guard_command, ci_contract)


if __name__ == "__main__":
    main()
