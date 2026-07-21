#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.public_api_manifest.v1"
POLICY = {
    "stable_core_source_compatibility": "required-within-0.3",
    "abi_compatibility": "not-guaranteed-before-1.0",
    "provisional_source_compatibility": "not-guaranteed",
    "platform_backend_headers": "unsupported-internal",
}
CATEGORIES = ("platform_internal", "provisional", "stable_core")


def normalize_cpp_public_surface(text: str) -> str:
    output: list[str] = []
    index = 0
    state = "code"
    quote = ""
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "line-comment":
            if char in "\r\n":
                output.append(" ")
                state = "code"
            index += 1
            continue
        if state == "block-comment":
            if char == "*" and following == "/":
                output.append(" ")
                state = "code"
                index += 2
            else:
                index += 1
            continue
        if state == "string":
            output.append(char)
            if char == "\\" and following:
                output.append(following)
                index += 2
                continue
            if char == quote:
                state = "code"
            index += 1
            continue
        if char == "/" and following == "/":
            state = "line-comment"
            index += 2
            continue
        if char == "/" and following == "*":
            state = "block-comment"
            index += 2
            continue
        if char in {'"', "'"}:
            state = "string"
            quote = char
        output.append(char)
        index += 1
    return " ".join("".join(output).split())


def header_fingerprint(path: Path) -> str:
    normalized = normalize_cpp_public_surface(path.read_text(encoding="utf-8"))
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()


def cmake_package_version(repo_root: Path) -> str:
    content = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(GameNetCore\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", content)
    if match is None:
        raise ValueError("cannot determine GameNetCore package version")
    return match.group(1)


def exported_targets(repo_root: Path) -> list[str]:
    aliases: dict[str, str] = {}
    installed: set[str] = set()
    cmake_files = [repo_root / "CMakeLists.txt"]
    cmake_files.extend(sorted((repo_root / "src").rglob("CMakeLists.txt")))
    for cmake_file in cmake_files:
        content = cmake_file.read_text(encoding="utf-8")
        for alias, concrete in re.findall(
            r"add_library\(\s*(GameNet::[A-Za-z0-9_]+)\s+ALIAS\s+([A-Za-z0-9_]+)\s*\)",
            content,
        ):
            aliases[alias] = concrete
        for target_group in re.findall(r"install\(TARGETS\s+([^\n\r\)]+)", content):
            installed.update(target_group.split())
    return sorted(alias for alias, concrete in aliases.items() if concrete in installed)


def _sorted_unique(values: Any, label: str, errors: list[str]) -> list[str]:
    if not isinstance(values, list) or not all(isinstance(value, str) for value in values):
        errors.append(f"{label} must be a string list")
        return []
    if values != sorted(values):
        errors.append(f"{label} must be sorted")
    if len(values) != len(set(values)):
        errors.append(f"{label} must not contain duplicates")
    return values


def verify_manifest(repo_root: Path, manifest_path: Path) -> list[str]:
    errors: list[str] = []
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot read manifest: {error}"]

    if manifest.get("schema") != SCHEMA:
        errors.append(f"schema must be {SCHEMA}")
    try:
        expected_version = cmake_package_version(repo_root)
    except ValueError as error:
        errors.append(str(error))
        expected_version = None
    if expected_version is not None and manifest.get("package_version") != expected_version:
        errors.append(
            f"package_version must match CMake ({expected_version}), got {manifest.get('package_version')!r}"
        )
    baseline = manifest.get("baseline")
    baseline_match = (
        re.fullmatch(r"v([0-9]+\.[0-9]+\.[0-9]+)-production-candidate", baseline)
        if isinstance(baseline, str)
        else None
    )
    if baseline_match is None:
        errors.append("baseline must identify a versioned production candidate")
    elif expected_version is not None and baseline_match.group(1) != expected_version:
        errors.append(
            f"baseline version must match CMake ({expected_version}), got {baseline!r}"
        )
    if manifest.get("policy") != POLICY:
        errors.append("compatibility policy changed without updating the verifier contract")

    declared_targets = _sorted_unique(manifest.get("exported_targets"), "exported_targets", errors)
    actual_targets = exported_targets(repo_root)
    if declared_targets != actual_targets:
        errors.append(f"exported target inventory mismatch: declared={declared_targets}, actual={actual_targets}")

    header_groups = manifest.get("headers")
    if not isinstance(header_groups, dict) or set(header_groups) != set(CATEGORIES):
        errors.append(f"headers must contain exactly: {', '.join(CATEGORIES)}")
        header_groups = {}
    classified: list[str] = []
    for category in CATEGORIES:
        values = _sorted_unique(header_groups.get(category), f"headers.{category}", errors)
        for value in values:
            candidate = Path(value)
            if candidate.is_absolute() or ".." in candidate.parts or not value.startswith("include/gamenet/"):
                errors.append(f"unsafe or non-public header path: {value}")
        classified.extend(values)

    if len(classified) != len(set(classified)):
        errors.append("public header categories overlap")
    actual_headers = sorted(
        path.relative_to(repo_root).as_posix()
        for path in (repo_root / "include" / "gamenet").rglob("*.h")
    )
    if sorted(classified) != actual_headers:
        missing = sorted(set(actual_headers) - set(classified))
        extra = sorted(set(classified) - set(actual_headers))
        errors.append(f"public header inventory mismatch: unclassified={missing}, missing_on_disk={extra}")

    stable_headers = header_groups.get("stable_core", [])
    fingerprints = manifest.get("stable_header_fingerprints")
    if not isinstance(fingerprints, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in fingerprints.items()
    ):
        errors.append("stable_header_fingerprints must be a string map")
        fingerprints = {}
    if list(fingerprints) != sorted(fingerprints):
        errors.append("stable_header_fingerprints must be sorted")
    if set(fingerprints) != set(stable_headers):
        errors.append("stable_header_fingerprints keys must exactly match headers.stable_core")
    for header in stable_headers:
        path = repo_root / header
        if not path.is_file():
            continue
        actual = header_fingerprint(path)
        if fingerprints.get(header) != actual:
            errors.append(f"stable public surface changed without manifest update: {header}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the versioned GameNet public API manifest")
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--print-stable-fingerprints", action="store_true")
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest or repo_root / "api" / "public_api_manifest.json"

    if args.print_stable_fingerprints:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        fingerprints = {
            header: header_fingerprint(repo_root / header)
            for header in manifest["headers"]["stable_core"]
        }
        print(json.dumps(fingerprints, indent=2, sort_keys=True))
        return 0

    errors = verify_manifest(repo_root, manifest_path)
    if errors:
        for error in errors:
            print(f"public API manifest error: {error}", file=sys.stderr)
        return 1
    print(f"public API manifest verified: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
