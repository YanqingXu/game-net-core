# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import copy
import hashlib
import json
import re
from pathlib import Path


EXPECTED_HEADERS = {
    "include/gamenet/experimental/io_uring/IoUringCompletionEngine.h",
    "include/gamenet/experimental/io_uring/IoUringEventLoopPump.h",
    "include/gamenet/experimental/io_uring/IoUringTcpClient.h",
    "include/gamenet/experimental/io_uring/IoUringTcpConnectionAdapter.h",
    "include/gamenet/experimental/io_uring/IoUringTcpConnectionHub.h",
    "include/gamenet/experimental/io_uring/IoUringTcpServer.h",
}


def canonical_sha256(path: Path) -> str:
    content = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(content).hexdigest()


def validate(repo_root: Path, manifest: dict[str, object]) -> list[str]:
    errors: list[str] = []
    if manifest.get("schema") != "gamenet.experimental_io_uring_api_manifest.v1":
        errors.append("schema")
    if manifest.get("component") != "experimental_io_uring":
        errors.append("component")
    if manifest.get("component_api_version") != "0.1.0":
        errors.append("component_api_version")
    if manifest.get("package_version") != "0.3.0":
        errors.append("package_version")
    if manifest.get("release_train") != "v0.5.0-experimental-preview":
        errors.append("release_train")
    if manifest.get("platform") != "Linux":
        errors.append("platform")
    if manifest.get("cmake_option") != "GAMENET_ENABLE_EXPERIMENTAL":
        errors.append("cmake_option")
    if manifest.get("default_enabled") is not False:
        errors.append("default_enabled")
    if manifest.get("target") != "GameNet::experimental_io_uring":
        errors.append("target")

    headers = manifest.get("headers")
    if not isinstance(headers, list) or headers != sorted(EXPECTED_HEADERS):
        errors.append("headers")
    fingerprints = manifest.get("header_fingerprints")
    if not isinstance(fingerprints, dict) or set(fingerprints) != EXPECTED_HEADERS:
        errors.append("header_fingerprints")
    else:
        for header in sorted(EXPECTED_HEADERS):
            expected = fingerprints.get(header)
            if not isinstance(expected, str) or re.fullmatch(r"[0-9a-f]{64}", expected) is None:
                errors.append(f"fingerprint-format:{header}")
                continue
            path = repo_root / header
            if not path.is_file() or canonical_sha256(path) != expected:
                errors.append(f"fingerprint:{header}")

    compatibility = manifest.get("compatibility")
    if compatibility != {
        "stable_manifest_member": False,
        "source_compatibility": "not-guaranteed-before-first-experimental-release",
        "abi_compatibility": "not-guaranteed",
        "automatic_backend_selection": False,
    }:
        errors.append("compatibility")
    return errors


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    manifest_path = repo_root / "api" / "experimental_io_uring_api_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert validate(repo_root, manifest) == []

    actual_headers = {
        path.relative_to(repo_root).as_posix()
        for path in (repo_root / "include" / "gamenet" / "experimental" / "io_uring").glob("*.h")
    }
    assert actual_headers == EXPECTED_HEADERS

    stable_manifest = json.loads(
        (repo_root / "api" / "public_api_manifest.json").read_text(encoding="utf-8")
    )
    stable_targets = {
        target
        for values in stable_manifest["targets"].values()
        for target in values
    }
    stable_headers = {
        header
        for values in stable_manifest["headers"].values()
        for header in values
    }
    assert "GameNet::experimental_io_uring" not in stable_targets
    assert not (EXPECTED_HEADERS & stable_headers)
    assert not any(
        header.startswith("include/gamenet/experimental/")
        for header in stable_headers
    )

    tampered = copy.deepcopy(manifest)
    tampered["header_fingerprints"][sorted(EXPECTED_HEADERS)[0]] = "0" * 64
    assert any(error.startswith("fingerprint:") for error in validate(repo_root, tampered))


if __name__ == "__main__":
    main()
