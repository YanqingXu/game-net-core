from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path


def load_scope_guard(repo_root: Path):
    script_path = repo_root / "tools" / "check_scope_boundaries.py"
    spec = importlib.util.spec_from_file_location("check_scope_boundaries", script_path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def assert_violation(repo_root: Path, relative_path: str, content: str, expected: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write(root / relative_path, content)
        guard = load_scope_guard(repo_root)
        violations = guard.check_scope(root)
        joined = "\n".join(str(item) for item in violations)
        assert expected in joined, joined


def assert_clean(repo_root: Path, relative_path: str, content: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write(root / relative_path, content)
        guard = load_scope_guard(repo_root)
        violations = guard.check_scope(root)
        assert violations == [], violations


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]

    assert_clean(
        repo_root,
        "include/gamenet/core/net/EventLoop.h",
        "namespace gamenet::net { class EventLoop {}; }\n",
    )
    assert_violation(
        repo_root,
        "src/core/net/Legacy.cc",
        '#include "mini/net/TcpConnection.h"\n',
        "legacy mini include",
    )
    assert_violation(
        repo_root,
        "src/core/net/Legacy.cc",
        "auto* p = mini::net::currentLoop();\n",
        "legacy mini namespace",
    )
    assert_violation(
        repo_root,
        "include/gamenet/protocol/PacketFramer.h",
        "namespace gamenet::protocol { class PacketFramer {}; }\n",
        "deferred path",
    )
    assert_violation(
        repo_root,
        "src/core/CMakeLists.txt",
        "add_library(gamenet_protocol protocol/PacketFramer.cc)\n",
        "deferred target",
    )


if __name__ == "__main__":
    main()
