from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing platform backend fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    core_cmake = repo_root / "src" / "core" / "CMakeLists.txt"
    poller_intent = repo_root / "intents" / "modules" / "poller.intent.md"
    platform_intent = repo_root / "intents" / "modules" / "platform_runtime.intent.md"
    sockets_ops = repo_root / "include" / "gamenet" / "core" / "net" / "SocketsOps.h"
    acceptor_header = repo_root / "include" / "gamenet" / "core" / "net" / "Acceptor.h"
    acceptor_source = repo_root / "src" / "core" / "net" / "Acceptor.cc"
    connector_source = repo_root / "src" / "core" / "net" / "Connector.cc"
    linux_sockets_source = repo_root / "src" / "core" / "net" / "platform" / "SocketsOps_linux.cc"
    linux_wakeup_source = repo_root / "src" / "core" / "net" / "platform" / "Wakeup_linux.cc"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    poller_include_dir = repo_root / "include" / "gamenet" / "core" / "net" / "poller"
    poller_source_dir = repo_root / "src" / "core" / "net" / "poller"
    legacy_select_header = poller_include_dir / "SelectPoller.h"
    legacy_select_source = poller_source_dir / "SelectPoller.cc"

    poller_text = poller_intent.read_text(encoding="utf-8")
    require(poller_text, "v1 backend targets are Linux epoll and Windows IOCP.", poller_intent)
    require(poller_text, "Windows WinSock `select()` is not an acceptable performance target", poller_intent)

    platform_text = platform_intent.read_text(encoding="utf-8")
    require(platform_text, "IOCP-backed Windows backend and `EPollPoller` on Linux", platform_intent)
    require(platform_text, "IOCP completion path", platform_intent)
    require(platform_text, "`*OrDie` compatibility helpers are not valid", platform_intent)
    assert "SelectPoller path" not in platform_text

    sockets_text = sockets_ops.read_text(encoding="utf-8")
    require(sockets_text, "SocketFd createNonblocking(sa_family_t family);", sockets_ops)
    require(sockets_text, "int bind(SocketFd sockfd", sockets_ops)
    require(sockets_text, "int listen(SocketFd sockfd);", sockets_ops)
    require(sockets_text, "bool tryGetLocalAddr", sockets_ops)
    require(sockets_text, "bool tryGetPeerAddr", sockets_ops)

    linux_sockets_text = linux_sockets_source.read_text(encoding="utf-8")
    require(linux_sockets_text, "MSG_NOSIGNAL", linux_sockets_source)
    require(linux_sockets_text, "errno == ENOTSOCK", linux_sockets_source)
    require(linux_sockets_text, "return ::write(sockfd, buffer, len);", linux_sockets_source)

    linux_wakeup_text = linux_wakeup_source.read_text(encoding="utf-8")
    require(linux_wakeup_text, "return ::write(fd, &one, sizeof(one));", linux_wakeup_source)

    acceptor_header_text = acceptor_header.read_text(encoding="utf-8")
    require(acceptor_header_text, "enum class AcceptorErrorAction", acceptor_header)
    require(acceptor_header_text, "void setErrorCallback", acceptor_header)

    acceptor_source_text = acceptor_source.read_text(encoding="utf-8")
    require(acceptor_source_text, "AcceptorErrorAction::Retry", acceptor_source)
    require(acceptor_source_text, "scheduleAcceptRetry", acceptor_source)
    assert "createNonblockingOrDie" not in acceptor_source_text
    assert "updateAcceptContextOrDie" not in acceptor_source_text

    connector_source_text = connector_source.read_text(encoding="utf-8")
    require(connector_source_text, "sockets::createNonblocking", connector_source)
    require(connector_source_text, "platform::createOverlappedTcp", connector_source)
    assert "createNonblockingOrDie" not in connector_source_text
    assert "bindUnspecifiedOrDie" not in connector_source_text
    assert "updateConnectContextOrDie" not in connector_source_text

    docs_text = ci_docs.read_text(encoding="utf-8")
    require(docs_text, "The active CI gate runs on `ubuntu-24.04` and `windows-latest`.", ci_docs)
    require(docs_text, "The Windows job validates the IOCP completion path", ci_docs)
    require(docs_text, "Windows MSVC Debug build", ci_docs)
    require(docs_text, "select-based", ci_docs)

    workflow_text = workflow.read_text(encoding="utf-8")
    cmake_text = core_cmake.read_text(encoding="utf-8")
    factory_source = poller_source_dir / "PollerFactory.cc"
    factory_text = factory_source.read_text(encoding="utf-8")

    require(factory_text, "IocpPoller", factory_source)
    assert "SelectPoller" not in factory_text
    require(cmake_text, "net/poller/IocpPoller.cc", core_cmake)
    assert "net/poller/SelectPoller.cc" not in cmake_text
    assert not legacy_select_header.exists(), "legacy SelectPoller public header must not be present"
    assert not legacy_select_source.exists(), "legacy SelectPoller source must not be present"

    if "windows-msvc:" in workflow_text:
        require(cmake_text, "Iocp", core_cmake)


if __name__ == "__main__":
    main()
