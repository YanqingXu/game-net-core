#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
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
            r"MetricsExporter|DnsResolver)\b"
        ),
    ),
)

COMPONENT_REFERENCE = re.compile(
    r"\b(?:namespace\s+gamenet::|gamenet_|GameNet::|gamenet/)"
    r"(protocol|transport|game_session|game_logic|broadcast|game|experimental)\b"
)

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
        if component in DEFERRED_COMPONENTS and component not in ACTIVE_COMPONENTS:
            violations.append(Violation(rel, 1, "deferred path", f"component {component!r} is not active"))

    if len(parts) >= 2 and parts[0] == "src":
        component = parts[1]
        if component in DEFERRED_SRC_DIRS and component not in ACTIVE_COMPONENTS:
            violations.append(Violation(rel, 1, "deferred path", f"component {component!r} is not active"))

    return violations


def check_text(root: Path, path: Path) -> list[Violation]:
    rel = path.relative_to(root).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")

    violations: list[Violation] = []
    layer = source_layer(rel)
    for line_number, line in enumerate(text.splitlines(), start=1):
        for match in COMPONENT_REFERENCE.finditer(line):
            component = match.group(1)
            if component not in ACTIVE_COMPONENTS:
                violations.append(Violation(rel, line_number, "deferred component", match.group(0)))
            if layer is not None and component not in LAYER_ALLOWED_COMPONENTS[layer]:
                violations.append(
                    Violation(rel, line_number, "layer references disallowed component", match.group(0))
                )
        for kind, pattern in TEXT_PATTERNS:
            match = pattern.search(line)
            if match is not None:
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
