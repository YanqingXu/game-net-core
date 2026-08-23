# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path

from run_release_consumer_gate import (
    V02_ASSET_NAME,
    V02_ASSET_SHA256,
    V02_COMMIT,
    V02_TAG,
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(evidence_dir: Path, expected_commit: str, expected_platform: str) -> None:
    evidence_dir = evidence_dir.resolve()
    manifest = json.loads((evidence_dir / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == "gamenet.release_consumer_evidence.v1"
    assert manifest["status"] == "success"
    assert manifest["candidate_sha"] == expected_commit
    assert re.fullmatch(r"[0-9a-f]{40}", manifest["source_tree_sha1"])
    assert manifest["platform"] == expected_platform
    assert manifest["backend"] == ("epoll" if expected_platform == "linux" else "iocp")
    assert manifest["configuration"] == "Release"
    expected_kind = f"{expected_platform}-x86_64"
    package = manifest["current_package"]
    assert package["kind"] == expected_kind
    assert package["file_count"] > 0
    package_path = evidence_dir / package["name"]
    assert package_path.is_file()
    assert package_path.stat().st_size == package["bytes"]
    assert sha256(package_path) == package["sha256"]

    assert manifest["v02_source"] == {
        "tag": V02_TAG,
        "commit": V02_COMMIT,
        "asset": V02_ASSET_NAME,
        "sha256": V02_ASSET_SHA256,
    }
    consumers = manifest["consumers"]
    assert [(item["name"], item["version"], item["tests"], item["status"]) for item in consumers] == [
        ("current-extracted", "0.3.0", "2/2", "success"),
        ("upgrade-v02", "0.2.0", "1/1", "success"),
        ("upgrade-v03", "0.3.0", "1/1", "success"),
    ]

    expected_files = {"manifest.json", package["name"]}
    for item in consumers:
        expected_count = int(item["tests"].split("/", maxsplit=1)[0])
        inventory_path = evidence_dir / item["inventory"]
        junit_path = evidence_dir / item["junit"]
        ctest_log_path = evidence_dir / item["ctest_log"]
        assert sha256(inventory_path) == item["inventory_sha256"]
        assert sha256(junit_path) == item["junit_sha256"]
        assert sha256(ctest_log_path) == item["ctest_log_sha256"]
        expected_files.update(
            {inventory_path.name, junit_path.name, ctest_log_path.name}
        )
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        assert inventory["schema"] == "gamenet.ctest_inventory.v1"
        assert inventory["total"] == inventory["expected_total"] == expected_count
        assert len(inventory["tests"]) == expected_count
        suite = ET.parse(junit_path).getroot()
        assert int(suite.attrib["tests"]) == expected_count
        assert int(suite.attrib["failures"]) == 0
        assert int(suite.attrib["disabled"]) == 0
        assert int(suite.attrib["skipped"]) == 0
        assert len(suite.findall("testcase")) == expected_count
        ctest_log = ctest_log_path.read_text(encoding="utf-8")
        assert "100% tests passed" in ctest_log
        assert f"0 tests failed out of {expected_count}" in ctest_log

    assert len(manifest["commands"]) == 15
    assert len(manifest["command_logs"]) == 15
    for item in manifest["command_logs"]:
        log_path = evidence_dir / item["path"]
        assert item["exit_code"] == 0
        assert sha256(log_path) == item["sha256"]
        expected_files.add(log_path.name)
    actual_files = {path.name for path in evidence_dir.iterdir() if path.is_file()}
    assert actual_files == expected_files


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify retained external release-consumer evidence.")
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--expected-platform", choices=("linux", "windows"), required=True)
    args = parser.parse_args()
    verify(args.evidence_dir, args.expected_commit, args.expected_platform)
    print(
        f"validated {args.expected_platform} release-consumer evidence for "
        f"{args.expected_commit}"
    )


if __name__ == "__main__":
    main()
