# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

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


def check_prestudy_boundaries(repo_root: Path) -> None:
    test_paths = {
        "tests/contract/protocol/test_packet_framer_view.cpp": {1},
        "tests/contract/runtime_model/test_spsc_mailbox.cpp": {1, 2},
        "tests/contract/event_loop/test_event_loop_mailbox_source.cpp": {1, 2},
        "tests/contract/io_engine/test_epoll_slot_dispatch.cpp": {3},
        "tests/contract/tcp_connection/test_output_segment_chain.cpp": {4},
        "tests/contract/tcp_connection/test_credit_lease.cpp": {5},
        "tests/contract/event_loop/test_adaptive_phase_scheduler.cpp": {6},
    }
    allowed_paths = {
        **{f"benchmarks/prestudy/hp{number}/Probe.cpp": {number} for number in range(1, 7)},
        **test_paths,
    }
    allowed_paths["benchmarks/prestudy/hp2/Probe.cpp"].add(1)
    for path, slices in allowed_paths.items():
        for number in slices:
            assert_clean(
                repo_root, path,
                f"namespace gamenet::experimental::hp{number} {{}}\n"
                f"using gamenet::experimental::hp{number}::Value;\n"
                f'#include "hp{number}/Probe.h"\n',
            )
        assert_violation(
            repo_root, path, "namespace gamenet::experimental::udp {}\n", "deferred component",
        )
        assert_violation(
            repo_root, path, "namespace gamenet::experimental::hp7 {}\n", "deferred component",
        )
        assert_violation(repo_root, path, "class WebSocket {};\n", "deferred high-level module")
        assert_violation(repo_root, path, '#include "mini/net/Buffer.h"\n', "legacy mini include")
        disallowed = next(number for number in range(1, 7) if number not in slices)
        assert_violation(
            repo_root, path, f"gamenet::experimental::hp{disallowed}::Value value;\n",
            "deferred component",
        )
        assert_violation(
            repo_root, path, f'#include "hp{disallowed}/Probe.h"\n', "disallowed prestudy include",
        )

    for path in (
        "benchmarks/prestudy/hp7/Probe.cpp",
        "benchmarks/prestudy/hp10/Probe.cpp",
        "benchmarks/prestudy/hp1_extra/Probe.cpp",
        "tests/contract/protocol/test_packet_framer_view_extra.cpp",
        "src/core/net/Probe.cc",
        "include/gamenet/core/net/Probe.h",
    ):
        assert_violation(
            repo_root, path, "gamenet::experimental::hp1::Value value;\n", "deferred component",
        )

    for include in (
        '"hp1/PacketFramerView.h"',
        '<hp1/PacketFramerView.h>',
        '"benchmarks/prestudy/hp1/PacketFramerView.h"',
        '"../../../benchmarks/prestudy/hp1/PacketFramerView.h"',
    ):
        assert_violation(
            repo_root, "src/core/net/Probe.cc", f"#include {include}\n",
            "disallowed prestudy include",
        )
    assert_violation(
        repo_root, "src/core/CMakeLists.txt",
        "target_link_libraries(gamenet_core PRIVATE gamenet_hp1_packet_framer_view)\n",
        "production references prestudy target",
    )
    assert_violation(
        repo_root, "include/gamenet/experimental/io_uring/Probe.h",
        "gamenet::experimental::hp1::Value value;\n", "deferred component",
    )


def check_status_description_boundary(repo_root: Path) -> None:
    path = "tests/cmake/test_migration_status_contract.py"
    for text in (
        'require(text, "M9 TLS / WebSocket / DNS", source)\n',
        'assert valid, ("M9 must not install TLS/WebSocket/DNS/HTTP headers: " + names)\n',
        'description = """M9\nWebSocket\nNO-PROMOTION"""\n',
    ):
        assert_clean(repo_root, path, text)
    for text in (
        'class WebSocket: pass\n',
        'description = "WebSocket"; WebSocket()\n',
        '# WebSocket implementation\n',
        'description = "WebSocket HttpServer"\n',
        'description = "WebSocket mini::net"\n',
        'description = f"WebSocket {WebSocket()}"\n',
    ):
        assert_violation(
            repo_root, path, text,
            "legacy mini namespace" if "mini::" in text else "deferred high-level module",
        )
    assert_violation(
        repo_root, "tests/cmake/test_unrelated.py", 'description = "WebSocket"\n',
        "deferred high-level module",
    )


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]

    assert_clean(
        repo_root,
        "include/gamenet/core/net/EventLoop.h",
        "namespace gamenet::net { class EventLoop {}; }\n",
    )
    assert_clean(
        repo_root,
        "include/gamenet/core/metrics/MetricsExporter.h",
        "namespace gamenet::metrics { class MetricsExporter {}; }\n",
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
    assert_clean(
        repo_root,
        "include/gamenet/protocol/PacketFramer.h",
        "namespace gamenet::protocol { class PacketFramer {}; }\n",
    )
    assert_clean(
        repo_root,
        "include/gamenet/game_session/SessionManager.h",
        '#include "gamenet/transport/TransportEndpoint.h"\n'
        "namespace gamenet::game_session { class SessionManager {}; }\n",
    )
    assert_violation(
        repo_root,
        "include/gamenet/protocol/BadDependency.h",
        '#include "gamenet/game_session/SessionManager.h"\n',
        "layer references disallowed component",
    )
    assert_violation(
        repo_root,
        "include/gamenet/experimental/Udp.h",
        "namespace gamenet::experimental { class Udp {}; }\n",
        "deferred path",
    )
    assert_clean(
        repo_root,
        "include/gamenet/experimental/io_uring/IoUringTcpServer.h",
        "namespace gamenet::experimental::io_uring { class IoUringTcpServer {}; }\n",
    )
    assert_clean(
        repo_root,
        "src/experimental/io_uring/OneShot.cc",
        "namespace gamenet::experimental::io_uring { class OneShot {}; }\n",
    )
    assert_clean(
        repo_root,
        "benchmarks/io_uring/listener_comparison.cpp",
        '#include "gamenet/experimental/io_uring/IoUringTcpServer.h"\n',
    )
    assert_violation(
        repo_root,
        "src/experimental/udp/Udp.cc",
        "namespace gamenet::experimental { class Udp {}; }\n",
        "deferred path",
    )
    assert_violation(
        repo_root,
        "src/core/CMakeLists.txt",
        "add_library(gamenet_protocol protocol/PacketFramer.cc)\n",
        "layer references disallowed component",
    )
    check_prestudy_boundaries(repo_root)
    check_status_description_boundary(repo_root)
    violations = load_scope_guard(repo_root).check_scope(repo_root)
    assert not violations, "\n".join(str(item) for item in violations)


if __name__ == "__main__":
    main()
