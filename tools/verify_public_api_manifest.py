#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from collections.abc import Iterable
from typing import Any


MANIFEST_SCHEMA = "gamenet.public_api_manifest.v2"
SNAPSHOT_SCHEMA = "gamenet.public_api_snapshot.v1"
POLICY = {
    "stable_core_source_compatibility": "required-within-compatibility-line",
    "abi_compatibility": "not-guaranteed-before-1.0",
    "provisional_source_compatibility": "not-guaranteed",
    "platform_backend_headers": "unsupported-internal",
}
CATEGORIES = ("platform_internal", "provisional", "stable_core")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
COMMIT_PATTERN = re.compile(r"[0-9a-f]{40}")


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
    return header_fingerprint_text(path.read_text(encoding="utf-8"))


def header_fingerprint_text(text: str) -> str:
    normalized = normalize_cpp_public_surface(text)
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()


def snapshot_content_sha256(text: str) -> str:
    canonical = text.replace("\r\n", "\n").replace("\r", "\n")
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def cmake_package_version(repo_root: Path) -> str:
    content = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(GameNetCore\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", content)
    if match is None:
        raise ValueError("cannot determine GameNetCore package version")
    return match.group(1)


def compatibility_line(package_version: str) -> str:
    match = re.fullmatch(r"([0-9]+)\.([0-9]+)\.[0-9]+", package_version)
    if match is None:
        raise ValueError(f"invalid semantic package version: {package_version!r}")
    return f"{match.group(1)}.{match.group(2)}"


def exported_targets_from_cmake_documents(documents: Iterable[str]) -> list[str]:
    aliases: dict[str, str] = {}
    installed: set[str] = set()
    for content in documents:
        for alias, concrete in re.findall(
            r"add_library\(\s*(GameNet::[A-Za-z0-9_]+)\s+ALIAS\s+([A-Za-z0-9_]+)\s*\)",
            content,
        ):
            aliases[alias] = concrete
        for target_group in re.findall(r"install\(TARGETS\s+([^\n\r\)]+)", content):
            installed.update(target_group.split())
    return sorted(alias for alias, concrete in aliases.items() if concrete in installed)


def exported_targets(repo_root: Path) -> list[str]:
    cmake_files = [repo_root / "CMakeLists.txt"]
    cmake_files.extend(sorted((repo_root / "src").rglob("CMakeLists.txt")))
    return exported_targets_from_cmake_documents(
        cmake_file.read_text(encoding="utf-8") for cmake_file in cmake_files
    )


def _read_json(path: Path, label: str, errors: list[str]) -> dict[str, Any] | None:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"cannot read {label}: {error}")
        return None
    if not isinstance(document, dict):
        errors.append(f"{label} must contain a JSON object")
        return None
    return document


def _sorted_unique(values: Any, label: str, errors: list[str]) -> list[str]:
    if not isinstance(values, list) or not all(isinstance(value, str) for value in values):
        errors.append(f"{label} must be a string list")
        return []
    if values != sorted(values):
        errors.append(f"{label} must be sorted")
    if len(values) != len(set(values)):
        errors.append(f"{label} must not contain duplicates")
    return values


def _classified_inventory(
    groups: Any,
    label: str,
    errors: list[str],
    *,
    kind: str,
) -> list[str]:
    if not isinstance(groups, dict) or set(groups) != set(CATEGORIES):
        errors.append(f"{label} must contain exactly: {', '.join(CATEGORIES)}")
        groups = {}
    classified: list[str] = []
    for category in CATEGORIES:
        values = _sorted_unique(groups.get(category), f"{label}.{category}", errors)
        for value in values:
            if kind == "target" and not re.fullmatch(r"GameNet::[A-Za-z0-9_]+", value):
                errors.append(f"invalid exported target name: {value}")
            if kind == "header":
                candidate = Path(value)
                if candidate.is_absolute() or ".." in candidate.parts or not value.startswith(
                    "include/gamenet/"
                ):
                    errors.append(f"unsafe or non-public header path: {value}")
        classified.extend(values)
    if len(classified) != len(set(classified)):
        errors.append(f"{label} categories overlap")
    return classified


def _verify_fingerprints(
    document: dict[str, Any],
    stable_headers: list[str],
    label: str,
    errors: list[str],
) -> dict[str, str]:
    fingerprints = document.get("stable_header_fingerprints")
    if not isinstance(fingerprints, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in fingerprints.items()
    ):
        errors.append(f"{label}.stable_header_fingerprints must be a string map")
        fingerprints = {}
    if list(fingerprints) != sorted(fingerprints):
        errors.append(f"{label}.stable_header_fingerprints must be sorted")
    if set(fingerprints) != set(stable_headers):
        errors.append(
            f"{label}.stable_header_fingerprints keys must exactly match headers.stable_core"
        )
    for header, fingerprint in fingerprints.items():
        if SHA256_PATTERN.fullmatch(fingerprint) is None:
            errors.append(f"{label} has an invalid stable fingerprint for {header}")
    return fingerprints


def _verify_snapshot(
    repo_root: Path,
    path: Path,
    reference: dict[str, Any],
    errors: list[str],
) -> None:
    snapshot = _read_json(path, "historical baseline snapshot", errors)
    if snapshot is None:
        return
    if snapshot.get("schema") != SNAPSHOT_SCHEMA:
        errors.append(f"historical baseline schema must be {SNAPSHOT_SCHEMA}")
    package_version = snapshot.get("package_version")
    if not isinstance(package_version, str):
        errors.append("historical baseline package_version must be a string")
    else:
        try:
            expected_line = compatibility_line(package_version)
        except ValueError as error:
            errors.append(str(error))
        else:
            if snapshot.get("compatibility_line") != expected_line:
                errors.append(
                    "historical baseline compatibility_line must match its package version"
                )
    source = snapshot.get("source")
    if not isinstance(source, dict) or set(source) != {"tag", "commit"}:
        errors.append("historical baseline source must contain exactly tag and commit")
    else:
        if source.get("tag") != reference.get("tag"):
            errors.append("historical baseline tag does not match manifest reference")
        if source.get("commit") != reference.get("commit"):
            errors.append("historical baseline commit does not match manifest reference")
    if snapshot.get("policy") != POLICY:
        errors.append("historical baseline compatibility policy does not match the verifier contract")

    targets = _classified_inventory(
        snapshot.get("targets"), "historical_baseline.targets", errors, kind="target"
    )
    if not targets:
        errors.append("historical baseline target inventory must not be empty")
    headers = _classified_inventory(
        snapshot.get("headers"), "historical_baseline.headers", errors, kind="header"
    )
    if not headers:
        errors.append("historical baseline header inventory must not be empty")
    header_groups = snapshot.get("headers")
    stable_headers = (
        header_groups.get("stable_core", []) if isinstance(header_groups, dict) else []
    )
    _verify_fingerprints(snapshot, stable_headers, "historical_baseline", errors)


def _run_git(
    repo_root: Path,
    arguments: list[str],
) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            ["git", "-C", str(repo_root), *arguments],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError:
        return None


def _git_object_text(
    repo_root: Path,
    commit: str,
    path: str,
    errors: list[str],
) -> str | None:
    result = _run_git(repo_root, ["show", f"{commit}:{path}"])
    if result is None or result.returncode != 0:
        detail = "git is unavailable" if result is None else result.stderr.strip()
        errors.append(
            f"cannot read historical Git object {commit}:{path}: {detail}"
        )
        return None
    return result.stdout


def _verify_snapshot_against_git(
    repo_root: Path,
    snapshot: dict[str, Any],
    commit: str,
    errors: list[str],
) -> None:
    tree = _run_git(
        repo_root,
        ["ls-tree", "-r", "--name-only", commit, "--", "include/gamenet", "src", "CMakeLists.txt"],
    )
    if tree is None or tree.returncode != 0:
        detail = "git is unavailable" if tree is None else tree.stderr.strip()
        errors.append(f"cannot enumerate historical Git tree {commit}: {detail}")
        return

    tree_paths = sorted(line for line in tree.stdout.splitlines() if line)
    actual_headers = sorted(
        path for path in tree_paths if path.startswith("include/gamenet/") and path.endswith(".h")
    )
    header_groups = snapshot.get("headers")
    declared_headers = sorted(
        header
        for category in CATEGORIES
        for header in (
            header_groups.get(category, []) if isinstance(header_groups, dict) else []
        )
        if isinstance(header, str)
    )
    if declared_headers != actual_headers:
        missing = sorted(set(actual_headers) - set(declared_headers))
        extra = sorted(set(declared_headers) - set(actual_headers))
        errors.append(
            "historical baseline Git header inventory mismatch: "
            f"unclassified={missing}, missing_at_commit={extra}"
        )

    stable_headers = (
        header_groups.get("stable_core", []) if isinstance(header_groups, dict) else []
    )
    fingerprints = snapshot.get("stable_header_fingerprints")
    if isinstance(fingerprints, dict):
        for header in stable_headers:
            if not isinstance(header, str) or header not in actual_headers:
                continue
            content = _git_object_text(repo_root, commit, header, errors)
            if content is None:
                continue
            actual = header_fingerprint_text(content)
            if fingerprints.get(header) != actual:
                errors.append(
                    "historical baseline stable fingerprint does not match "
                    f"{commit}:{header}"
                )

    cmake_paths = sorted(
        path
        for path in tree_paths
        if path == "CMakeLists.txt" or path.endswith("/CMakeLists.txt")
    )
    cmake_documents: list[str] = []
    root_cmake: str | None = None
    for cmake_path in cmake_paths:
        content = _git_object_text(repo_root, commit, cmake_path, errors)
        if content is None:
            continue
        cmake_documents.append(content)
        if cmake_path == "CMakeLists.txt":
            root_cmake = content

    if root_cmake is not None:
        version_match = re.search(
            r"project\(GameNetCore\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
            root_cmake,
        )
        if version_match is None:
            errors.append(
                f"cannot determine historical package version from {commit}:CMakeLists.txt"
            )
        elif snapshot.get("package_version") != version_match.group(1):
            errors.append(
                "historical baseline package version does not match Git commit: "
                f"declared={snapshot.get('package_version')!r}, "
                f"actual={version_match.group(1)!r}"
            )

    target_groups = snapshot.get("targets")
    declared_targets = sorted(
        target
        for category in CATEGORIES
        for target in (
            target_groups.get(category, []) if isinstance(target_groups, dict) else []
        )
        if isinstance(target, str)
    )
    actual_targets = exported_targets_from_cmake_documents(cmake_documents)
    if declared_targets != actual_targets:
        errors.append(
            "historical baseline Git target inventory mismatch: "
            f"declared={declared_targets}, actual={actual_targets}"
        )


def _verify_historical_reference(
    repo_root: Path,
    reference: Any,
    errors: list[str],
    *,
    require_git_baseline: bool,
) -> None:
    expected_keys = {"tag", "commit", "path", "snapshot_sha256"}
    if not isinstance(reference, dict) or set(reference) != expected_keys:
        errors.append(
            "historical_baseline must contain exactly tag, commit, path, and snapshot_sha256"
        )
        return
    tag = reference.get("tag")
    commit = reference.get("commit")
    path_text = reference.get("path")
    snapshot_sha256 = reference.get("snapshot_sha256")
    if not isinstance(tag, str) or re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+[-A-Za-z0-9.]*", tag) is None:
        errors.append("historical_baseline.tag must be a versioned tag")
    if not isinstance(commit, str) or COMMIT_PATTERN.fullmatch(commit) is None:
        errors.append("historical_baseline.commit must be a lowercase full commit SHA")
    if not isinstance(path_text, str):
        errors.append("historical_baseline.path must be a repository-relative string")
        return
    relative_path = Path(path_text)
    if (
        relative_path.is_absolute()
        or ".." in relative_path.parts
        or relative_path.as_posix() != path_text
        or not path_text.startswith("api/baselines/")
    ):
        errors.append("historical_baseline.path must stay under api/baselines")
        return
    if not isinstance(snapshot_sha256, str) or SHA256_PATTERN.fullmatch(snapshot_sha256) is None:
        errors.append("historical_baseline.snapshot_sha256 must be a lowercase SHA-256")
    snapshot_path = repo_root / relative_path
    try:
        snapshot_text = snapshot_path.read_text(encoding="utf-8")
    except OSError as error:
        errors.append(f"cannot read historical baseline snapshot: {error}")
        return
    actual_sha256 = snapshot_content_sha256(snapshot_text)
    if snapshot_sha256 != actual_sha256:
        errors.append(
            "historical baseline snapshot hash mismatch: "
            f"declared={snapshot_sha256}, actual={actual_sha256}"
        )
    _verify_snapshot(repo_root, snapshot_path, reference, errors)

    snapshot = _read_json(snapshot_path, "historical baseline snapshot", errors)

    # The checked-in hash makes the snapshot portable, while a full-history
    # checkout proves that the snapshot inventory and stable fingerprints were
    # actually derived from the immutable tagged Git object.
    if isinstance(tag, str) and isinstance(commit, str):
        resolved = _run_git(repo_root, ["rev-parse", "--verify", f"{tag}^{{commit}}"])
        if resolved is None or resolved.returncode != 0:
            if require_git_baseline:
                detail = "git is unavailable" if resolved is None else resolved.stderr.strip()
                errors.append(
                    f"historical baseline tag {tag} is unavailable for required Git verification: "
                    f"{detail}"
                )
        elif resolved.stdout.strip() != commit:
            errors.append(
                f"historical baseline tag {tag} resolves to {resolved.stdout.strip()}, "
                f"not {commit}"
            )
        elif snapshot is not None:
            _verify_snapshot_against_git(repo_root, snapshot, commit, errors)


def verify_manifest(
    repo_root: Path,
    manifest_path: Path,
    *,
    require_git_baseline: bool = False,
) -> list[str]:
    errors: list[str] = []
    manifest = _read_json(manifest_path, "manifest", errors)
    if manifest is None:
        return errors

    if manifest.get("schema") != MANIFEST_SCHEMA:
        errors.append(f"schema must be {MANIFEST_SCHEMA}")
    try:
        expected_version = cmake_package_version(repo_root)
    except ValueError as error:
        errors.append(str(error))
        expected_version = None
    if expected_version is not None and manifest.get("package_version") != expected_version:
        errors.append(
            f"package_version must match CMake ({expected_version}), "
            f"got {manifest.get('package_version')!r}"
        )
    if expected_version is not None:
        expected_line = compatibility_line(expected_version)
        if manifest.get("compatibility_line") != expected_line:
            errors.append(
                f"compatibility_line must match CMake major.minor ({expected_line}), "
                f"got {manifest.get('compatibility_line')!r}"
            )
    release_label = manifest.get("release_label")
    release_match = (
        re.fullmatch(r"v([0-9]+\.[0-9]+\.[0-9]+)-production-candidate", release_label)
        if isinstance(release_label, str)
        else None
    )
    if release_match is None:
        errors.append("release_label must identify a versioned production candidate")
    elif expected_version is not None and release_match.group(1) != expected_version:
        errors.append(
            f"release_label version must match CMake ({expected_version}), got {release_label!r}"
        )
    if manifest.get("policy") != POLICY:
        errors.append("compatibility policy changed without updating the verifier contract")

    declared_targets = _classified_inventory(
        manifest.get("targets"), "targets", errors, kind="target"
    )
    actual_targets = exported_targets(repo_root)
    if sorted(declared_targets) != actual_targets:
        errors.append(
            f"exported target inventory mismatch: declared={sorted(declared_targets)}, "
            f"actual={actual_targets}"
        )

    header_groups = manifest.get("headers")
    classified_headers = _classified_inventory(
        header_groups, "headers", errors, kind="header"
    )
    actual_headers = sorted(
        path.relative_to(repo_root).as_posix()
        for path in (repo_root / "include" / "gamenet").rglob("*.h")
    )
    if sorted(classified_headers) != actual_headers:
        missing = sorted(set(actual_headers) - set(classified_headers))
        extra = sorted(set(classified_headers) - set(actual_headers))
        errors.append(
            f"public header inventory mismatch: unclassified={missing}, missing_on_disk={extra}"
        )

    stable_headers = (
        header_groups.get("stable_core", []) if isinstance(header_groups, dict) else []
    )
    fingerprints = _verify_fingerprints(manifest, stable_headers, "manifest", errors)
    for header in stable_headers:
        path = repo_root / header
        if not path.is_file():
            continue
        actual = header_fingerprint(path)
        if fingerprints.get(header) != actual:
            errors.append(f"stable public surface changed without manifest update: {header}")

    _verify_historical_reference(
        repo_root,
        manifest.get("historical_baseline"),
        errors,
        require_git_baseline=require_git_baseline,
    )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the versioned GameNet public API manifest")
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--print-stable-fingerprints", action="store_true")
    parser.add_argument(
        "--require-git-baseline",
        action="store_true",
        help="require the historical tag object and verify the snapshot against its Git tree",
    )
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

    errors = verify_manifest(
        repo_root,
        manifest_path,
        require_git_baseline=args.require_git_baseline,
    )
    if errors:
        for error in errors:
            print(f"public API manifest error: {error}", file=sys.stderr)
        return 1
    print(f"public API manifest verified: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
