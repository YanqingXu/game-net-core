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

    poller_text = poller_intent.read_text(encoding="utf-8")
    require(poller_text, "v1 backend targets are Linux epoll and Windows IOCP.", poller_intent)
    require(poller_text, "Windows WinSock `select()` is not an acceptable performance target", poller_intent)

    platform_text = platform_intent.read_text(encoding="utf-8")
    require(platform_text, "IOCP-backed Windows backend and `EPollPoller` on Linux", platform_intent)
    require(platform_text, "IOCP completion path", platform_intent)
    assert "SelectPoller path" not in platform_text

    docs_text = ci_docs.read_text(encoding="utf-8")
    require(docs_text, "Windows CI is intentionally deferred until the Windows network backend is IOCP", ci_docs)
    require(docs_text, "must not freeze the old select-based backend", ci_docs)

    workflow_text = workflow.read_text(encoding="utf-8")
    cmake_text = core_cmake.read_text(encoding="utf-8")

    if "windows-cmake:" in workflow_text:
        require(cmake_text, "Iocp", core_cmake)
        assert "SelectPoller.cc" not in cmake_text


if __name__ == "__main__":
    main()
