#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
HASH_SUFFIXES = {".cmake", ".in", ".py", ".ps1", ".sh", ".yaml", ".yml"}
SPDX_IDENTIFIER = "SPDX-License-Identifier: Apache-2.0"
COPYRIGHT = "Copyright 2026 Yanqing Xu"


def tracked_and_untracked_files(repo_root: Path) -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
        ],
        cwd=repo_root,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr.decode("utf-8", errors="replace"))
    return [
        repo_root / entry.decode("utf-8")
        for entry in result.stdout.split(b"\0")
        if entry
    ]


def comment_prefix(path: Path) -> str | None:
    if path.suffix.lower() in CPP_SUFFIXES:
        return "//"
    if path.name == "CMakeLists.txt" or path.suffix.lower() in HASH_SUFFIXES:
        return "#"
    return None


def expected_header(prefix: str) -> tuple[str, str]:
    return f"{prefix} {COPYRIGHT}", f"{prefix} {SPDX_IDENTIFIER}"


def header_offset(path: Path, lines: list[str]) -> int:
    if path.suffix.lower() in {".py", ".sh"} and lines and lines[0].startswith("#!"):
        return 1
    return 0


def validate_file(path: Path) -> str | None:
    prefix = comment_prefix(path)
    if prefix is None:
        return None
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    offset = header_offset(path, lines)
    expected = expected_header(prefix)
    actual = tuple(lines[offset : offset + 2])
    if actual != expected:
        return f"{path}: expected {expected!r} at line {offset + 1}, got {actual!r}"
    spdx_header_lines = [
        line for line in lines if line in {f"// {SPDX_IDENTIFIER}", f"# {SPDX_IDENTIFIER}"}
    ]
    if len(spdx_header_lines) != 1:
        return f"{path}: expected exactly one {SPDX_IDENTIFIER!r}"
    return None


def apply_header(path: Path) -> bool:
    prefix = comment_prefix(path)
    if prefix is None:
        return False
    text = path.read_text(encoding="utf-8")
    if SPDX_IDENTIFIER in text:
        error = validate_file(path)
        if error is not None:
            raise SystemExit(error)
        return False
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)
    offset = header_offset(path, [line.rstrip("\r\n") for line in lines])
    copyright_line, spdx_line = expected_header(prefix)
    lines[offset:offset] = [copyright_line + newline, spdx_line + newline, newline]
    path.write_text("".join(lines), encoding="utf-8", newline="")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply or verify canonical Apache-2.0 source headers"
    )
    parser.add_argument("--apply", action="store_true", help="add missing headers")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    eligible = sorted(
        path
        for path in tracked_and_untracked_files(repo_root)
        if path.is_file() and comment_prefix(path) is not None
    )
    changed = 0
    if args.apply:
        changed = sum(apply_header(path) for path in eligible)

    errors = [error for path in eligible if (error := validate_file(path)) is not None]
    if errors:
        raise SystemExit("\n".join(errors))
    print(
        f"validated {len(eligible)} Apache-2.0 source headers"
        + (f"; added {changed}" if args.apply else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
