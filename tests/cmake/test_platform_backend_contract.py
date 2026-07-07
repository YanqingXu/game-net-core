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
    assert "SelectPoller path" not in platform_text

    docs_text = ci_docs.read_text(encoding="utf-8")
    require(docs_text, "Windows CI is intentionally deferred until the local IOCP runtime evidence", ci_docs)
    require(docs_text, "Windows install/package consumer verification", ci_docs)
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

    if "windows-cmake:" in workflow_text:
        require(cmake_text, "Iocp", core_cmake)


if __name__ == "__main__":
    main()
