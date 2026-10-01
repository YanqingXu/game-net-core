#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import io
import re
import sys
import tokenize
from dataclasses import dataclass
from pathlib import Path


ACTIVE_COMPONENTS = {"core", "protocol", "transport", "game_session", "game_logic", "broadcast"}
DEFERRED_COMPONENTS = {"protocol", "transport", "game", "experimental"}
DEFERRED_SRC_DIRS = {"protocol", "transport", "experimental", "broadcast", "game_logic", "game_session"}
SCAN_ENTRIES = ("include", "src", "tests", "examples", "benchmarks", "cmake", ".github", "CMakeLists.txt")
SKIP_DIRS = {".git", "build", "out", "mini_trantor"}
SKIP_FILES = {
    "tests/scope/test_scope_guard.py",
    "tests/scope/test_intent_metadata.py",
}
ACTIVE_EXPERIMENTAL_SOURCE_PREFIX = "src/experimental/io_uring/"
ACTIVE_EXPERIMENTAL_HEADER_PREFIX = "include/gamenet/experimental/io_uring/"
ACTIVE_EXPERIMENTAL_REFERENCE_PREFIXES = (
    ACTIVE_EXPERIMENTAL_SOURCE_PREFIX,
    ACTIVE_EXPERIMENTAL_HEADER_PREFIX,
    "benchmarks/io_uring/",
    "tests/contract/io_engine/test_io_uring_",
    "tests/cmake/experimental_io_uring_install_consumer/",
)
ACTIVE_EXPERIMENTAL_TEST_PATHS = {
    "benchmarks/CMakeLists.txt",
    "benchmarks/io_uring/one_shot.cpp",
    "tests/CMakeLists.txt",
    "tests/cmake/test_io_uring_completion_engine_contract.py",
    "tests/cmake/test_experimental_io_uring_install_contract.py",
    "tests/api/test_experimental_io_uring_api_manifest.py",
    "tests/api/test_public_api_manifest.py",
    "tests/contract/io_engine/test_io_uring_completion_engine.cpp",
    "tests/contract/io_engine/test_cross_backend_tcp_semantics.cpp",
}
IO_URING_BOUNDARY_LINES = {
    "src/experimental/io_uring/CMakeLists.txt": {
        "DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet/experimental",
    },
    "tests/api/test_experimental_io_uring_api_manifest.py": {
        'header.startswith("include/gamenet/experimental/")',
    },
    "tests/cmake/test_experimental_io_uring_install_contract.py": {
        'assert re.search(r"GameNet::experimental(?!_io_uring)", target_text) is None',
        'assert re.search(r"GameNet::experimental(?!_io_uring)", cmake_text) is None',
    },
}
# These are concluded, opt-in HP1-HP6 prestudies, not a promotion of the
# experimental component. HP2's owned packet carrier is the explicit HP1 edge.
PRESTUDY_SOURCE_SLICES = {
    "benchmarks/prestudy/hp1/": {"hp1"},
    "benchmarks/prestudy/hp2/": {"hp1", "hp2"},
    "benchmarks/prestudy/hp3/": {"hp3"},
    "benchmarks/prestudy/hp4/": {"hp4"},
    "benchmarks/prestudy/hp5/": {"hp5"},
    "benchmarks/prestudy/hp6/": {"hp6"},
}
PRESTUDY_TEST_SLICES = {
    "tests/contract/protocol/test_packet_framer_view.cpp": {"hp1"},
    "tests/contract/runtime_model/test_spsc_mailbox.cpp": {"hp1", "hp2"},
    "tests/contract/event_loop/test_event_loop_mailbox_source.cpp": {"hp1", "hp2"},
    "tests/contract/io_engine/test_epoll_slot_dispatch.cpp": {"hp3"},
    "tests/contract/tcp_connection/test_output_segment_chain.cpp": {"hp4"},
    "tests/contract/tcp_connection/test_credit_lease.cpp": {"hp5"},
    "tests/contract/event_loop/test_adaptive_phase_scheduler.cpp": {"hp6"},
}
TEXT_SUFFIXES = {
    "",
    ".cmake",
    ".cpp",
    ".cc",
    ".cxx",
    ".h",
    ".hpp",
    ".hxx",
    ".md",
    ".py",
    ".txt",
    ".yml",
    ".yaml",
}


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    kind: str
    detail: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.kind}: {self.detail}"


TEXT_PATTERNS = (
    ("legacy mini include", re.compile(r'#\s*include\s*[<"]mini/')),
    ("legacy mini namespace", re.compile(r"\bmini::|\bnamespace\s+mini\b")),
    (
        "deferred high-level module",
        re.compile(
            r"\b(?:HttpServer|HttpClient|WebSocket|Rpc[A-Z]\w*|Kcp[A-Z]\w*|"
            r"Tls[A-Z]\w*|UdpSocket|"
            r"DnsResolver)\b"
        ),
    ),
)

COMPONENT_REFERENCE = re.compile(
    r"\b(?:gamenet::|gamenet_|GameNet::|gamenet/)"
    r"(protocol|transport|game_session|game_logic|broadcast|game|experimental)\b"
)
PRESTUDY_NAMESPACE = re.compile(r"::(hp[1-6])\b")
PRESTUDY_TARGET = re.compile(r"\bgamenet_hp\d+_\w+\b")
INCLUDE_PATH = re.compile(r'#\s*include\s*[<"]([^>"\n]+)[>"]')
PRESTUDY_INCLUDE = re.compile(r"(?:^|/)(hp\d+)/")

LAYER_ALLOWED_COMPONENTS = {
    "core": set(),
    "protocol": {"protocol"},
    "transport": {"transport"},
    "game_session": {"game_session", "transport"},
    "game_logic": {"game_logic", "game_session", "transport"},
    "broadcast": {"broadcast", "game_session", "transport"},
}


def source_layer(rel: str) -> str | None:
    parts = Path(rel).parts
    if len(parts) >= 3 and parts[0] == "include" and parts[1] == "gamenet":
        return parts[2] if parts[2] in LAYER_ALLOWED_COMPONENTS else None
    if len(parts) >= 2 and parts[0] == "src":
        return parts[1] if parts[1] in LAYER_ALLOWED_COMPONENTS else None
    return None


def is_text_file(path: Path) -> bool:
    return path.suffix in TEXT_SUFFIXES


def iter_scan_files(root: Path):
    for entry in SCAN_ENTRIES:
        path = root / entry
        if not path.exists():
            continue
        if path.is_file():
            if path.relative_to(root).as_posix() not in SKIP_FILES and is_text_file(path):
                yield path
            continue
        for candidate in path.rglob("*"):
            if candidate.is_dir():
                continue
            rel = candidate.relative_to(root)
            if rel.as_posix() in SKIP_FILES:
                continue
            if any(part in SKIP_DIRS for part in rel.parts):
                continue
            if is_text_file(candidate):
                yield candidate


def check_deferred_path(root: Path, path: Path) -> list[Violation]:
    rel = path.relative_to(root).as_posix()
    parts = path.relative_to(root).parts
    violations: list[Violation] = []

    if len(parts) >= 3 and parts[0] == "include" and parts[1] == "gamenet":
        component = parts[2]
        if component == "experimental" and rel.startswith(ACTIVE_EXPERIMENTAL_HEADER_PREFIX):
            return violations
        if component in DEFERRED_COMPONENTS and component not in ACTIVE_COMPONENTS:
            violations.append(Violation(rel, 1, "deferred path", f"component {component!r} is not active"))

    if len(parts) >= 2 and parts[0] == "src":
        component = parts[1]
        if component == "experimental" and rel.startswith(ACTIVE_EXPERIMENTAL_SOURCE_PREFIX):
            return violations
        if component in DEFERRED_SRC_DIRS and component not in ACTIVE_COMPONENTS:
            violations.append(Violation(rel, 1, "deferred path", f"component {component!r} is not active"))

    return violations


def allowed_prestudy_slices(rel: str) -> set[str]:
    for prefix, slices in PRESTUDY_SOURCE_SLICES.items():
        if rel.startswith(prefix):
            return slices
    return PRESTUDY_TEST_SLICES.get(rel, set())


def status_description_tokens(rel: str, text: str) -> list[tokenize.TokenInfo]:
    if rel != "tests/cmake/test_migration_status_contract.py":
        return []
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, IndentationError):
        return []
    return [
        token for token in tokens
        if token.type == tokenize.STRING
        # Older Python versions tokenize an entire f-string as STRING, including
        # executable expressions. Such strings must not exempt identifiers.
        and "f" not in re.match(r"[a-zA-Z]*", token.string).group().lower()
    ]


def check_text(root: Path, path: Path) -> list[Violation]:
    rel = path.relative_to(root).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")

    violations: list[Violation] = []
    layer = source_layer(rel)
    prestudy_slices = allowed_prestudy_slices(rel)
    description_tokens = status_description_tokens(rel, text)
    for line_number, line in enumerate(text.splitlines(), start=1):
        for match in COMPONENT_REFERENCE.finditer(line):
            component = match.group(1)
            suffix = line[match.end():]
            active_io_uring_reference = (
                component == "experimental"
                and (
                    re.match(r"(?:::|/)io_uring\b", suffix) is not None
                    or line.strip() in IO_URING_BOUNDARY_LINES.get(rel, set())
                )
                and (
                    any(rel.startswith(prefix) for prefix in ACTIVE_EXPERIMENTAL_REFERENCE_PREFIXES)
                    or rel in ACTIVE_EXPERIMENTAL_TEST_PATHS
                )
            )
            prestudy_namespace = PRESTUDY_NAMESPACE.match(suffix)
            active_prestudy_reference = (
                component == "experimental"
                and prestudy_namespace is not None
                and prestudy_namespace.group(1) in prestudy_slices
            )
            if active_io_uring_reference or active_prestudy_reference:
                continue
            if component not in ACTIVE_COMPONENTS:
                violations.append(Violation(rel, line_number, "deferred component", match.group(0)))
            if layer is not None and component not in LAYER_ALLOWED_COMPONENTS[layer]:
                violations.append(
                    Violation(rel, line_number, "layer references disallowed component", match.group(0))
                )
        for include in INCLUDE_PATH.finditer(line):
            include_path = include.group(1).replace("\\", "/")
            slices = set(PRESTUDY_INCLUDE.findall(include_path))
            if slices - prestudy_slices or ("prestudy/" in include_path and not slices):
                violations.append(
                    Violation(rel, line_number, "disallowed prestudy include", include.group(1))
                )
        if layer is not None:
            for match in PRESTUDY_TARGET.finditer(line):
                violations.append(
                    Violation(rel, line_number, "production references prestudy target", match.group(0))
                )
        for kind, pattern in TEXT_PATTERNS:
            for match in pattern.finditer(line):
                if kind == "deferred high-level module" and match.group(0) == "WebSocket":
                    if any(
                        token.start <= (line_number, match.start())
                        and (line_number, match.end()) <= token.end
                        for token in description_tokens
                    ):
                        continue
                violations.append(Violation(rel, line_number, kind, match.group(0)))
    return violations


def check_scope(root: Path) -> list[Violation]:
    root = root.resolve()
    violations: list[Violation] = []
    for path in iter_scan_files(root):
        violations.extend(check_deferred_path(root, path))
        violations.extend(check_text(root, path))
    return violations


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check game-net-core migration scope boundaries.")
    parser.add_argument("--root", default=".", help="Repository root to scan.")
    args = parser.parse_args(argv)

    violations = check_scope(Path(args.root))
    if violations:
        print("Scope boundary violations found:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        print(
            "\nIf this is an intentional Phase 4 promotion, update the active component scope "
            "and migration status in the same change.",
            file=sys.stderr,
        )
        return 1

    print("Scope boundary check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
