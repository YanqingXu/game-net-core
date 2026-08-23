# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import io
import hashlib
import json
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing release-consumer fragment in {source}: {needle}"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def create_evidence_fixture(root: Path, commit: str) -> None:
    package = root / "game-net-core-v0.3.0-linux-x86_64.tar.gz"
    package.write_bytes(b"package")
    command_logs = []
    for index in range(15):
        path = root / f"command-{index}.log"
        path.write_text("exit_code: 0\n", encoding="utf-8")
        command_logs.append(
            {"path": path.name, "sha256": sha256(path), "exit_code": 0}
        )
    consumers = []
    for name, version, count in (
        ("current-extracted", "0.3.0", 2),
        ("upgrade-v02", "0.2.0", 1),
        ("upgrade-v03", "0.3.0", 1),
    ):
        inventory = root / f"{name}-inventory.json"
        inventory.write_text(
            json.dumps(
                {
                    "schema": "gamenet.ctest_inventory.v1",
                    "total": count,
                    "expected_total": count,
                    "tests": [{"name": f"test-{index}"} for index in range(count)],
                }
            )
            + "\n",
            encoding="utf-8",
        )
        junit = root / f"{name}-junit.xml"
        cases = "".join(f'<testcase name="test-{index}"/>' for index in range(count))
        junit.write_text(
            f'<testsuite tests="{count}" failures="0" disabled="0" skipped="0">'
            f"{cases}</testsuite>\n",
            encoding="utf-8",
        )
        ctest_log = root / f"{name}-ctest.log"
        failure_summary = "" if name == "current-extracted" else ", 0 tests failed"
        ctest_log.write_text(
            f"100% tests passed{failure_summary} out of {count}\n", encoding="utf-8"
        )
        consumers.append(
            {
                "name": name,
                "version": version,
                "tests": f"{count}/{count}",
                "status": "success",
                "inventory": inventory.name,
                "inventory_sha256": sha256(inventory),
                "junit": junit.name,
                "junit_sha256": sha256(junit),
                "ctest_log": ctest_log.name,
                "ctest_log_sha256": sha256(ctest_log),
            }
        )
    manifest = {
        "schema": "gamenet.release_consumer_evidence.v1",
        "status": "success",
        "candidate_sha": commit,
        "source_tree_sha1": "b" * 40,
        "platform": "linux",
        "backend": "epoll",
        "configuration": "Release",
        "current_package": {
            "kind": "linux-x86_64",
            "name": package.name,
            "sha256": sha256(package),
            "bytes": package.stat().st_size,
            "file_count": 1,
        },
        "v02_source": {
            "tag": "v0.2.0-phase4-preview",
            "commit": "7668d6b82a0d815ccd79f83c572bc0a36bcceea0",
            "asset": "game-net-core-v0.2.0-phase4-preview.tar.gz",
            "sha256": "c2f5f2ece4a147f63f91c86ef3c1dd25bf9d370d22e25889648132985f9af408",
        },
        "consumers": consumers,
        "commands": [["fixture"] for _ in range(15)],
        "command_logs": command_logs,
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    tool = repo_root / "tools" / "run_release_consumer_gate.py"
    verifier = repo_root / "tools" / "verify_release_consumer_evidence.py"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    consumer_cmake = repo_root / "tests" / "cmake" / "upgrade_consumer" / "CMakeLists.txt"
    consumer_source = consumer_cmake.with_name("main.cpp")
    release_safe_guard = repo_root / "tests" / "cmake" / "test_release_safe_tests.py"
    tool_text = tool.read_text(encoding="utf-8")
    verifier_text = verifier.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    cmake_text = consumer_cmake.read_text(encoding="utf-8")
    source_text = consumer_source.read_text(encoding="utf-8")
    release_safe_text = release_safe_guard.read_text(encoding="utf-8")

    for fragment in (
        "c2f5f2ece4a147f63f91c86ef3c1dd25bf9d370d22e25889648132985f9af408",
        "7668d6b82a0d815ccd79f83c572bc0a36bcceea0",
        "gamenet.release_consumer_evidence.v1",
        "current-extracted",
        "upgrade-v02",
        "upgrade-v03",
        "consumer gate candidate must be the exact checked-out HEAD commit",
        "v0.2 canonical source archive SHA256 mismatch",
    ):
        require(tool_text, fragment, tool)
    for fragment in (
        "GAMENET_EXPECTED_VERSION is required",
        "find_package(GameNetCore ${GAMENET_EXPECTED_VERSION} EXACT REQUIRED)",
        "GameNetCore 0.3.0 package license mismatch",
        "GameNet::core",
    ):
        require(cmake_text, fragment, consumer_cmake)
    for fragment in (
        "gamenet/core/base/Timestamp.h",
        "gamenet/core/net/Buffer.h",
        "gamenet/core/net/InetAddress.h",
        'buffer.append("upgrade", 7)',
    ):
        require(source_text, fragment, consumer_source)
    require(
        release_safe_text,
        'and "upgrade_consumer" not in path.relative_to(repo_root).parts',
        release_safe_guard,
    )
    assert workflow_text.count("python3 tools/run_release_consumer_gate.py") == 2
    assert workflow_text.count("python tools/run_release_consumer_gate.py") == 2
    assert workflow_text.count("python3 tools/verify_release_consumer_evidence.py") == 2
    assert workflow_text.count("python tools/verify_release_consumer_evidence.py") == 2
    assert workflow_text.count(V02_DOWNLOAD := "game-net-core-v0.2.0-phase4-preview.tar.gz") >= 2
    assert V02_DOWNLOAD in tool_text
    for fragment in (
        "gamenet.release_consumer_evidence.v1",
        "current-extracted",
        "upgrade-v02",
        "upgrade-v03",
        "100% tests passed",
        "(?:, 0 tests failed)?",
        "actual_files == expected_files",
    ):
        require(verifier_text, fragment, verifier)

    sys.path.insert(0, str(repo_root / "tools"))
    from run_release_consumer_gate import extract_archive

    with tempfile.TemporaryDirectory(prefix="gamenet-consumer-archive-") as temporary:
        root = Path(temporary)
        malicious_tar = root / "malicious.tar.gz"
        with tarfile.open(malicious_tar, mode="w:gz") as archive:
            info = tarfile.TarInfo("../escape")
            info.size = 1
            archive.addfile(info, io.BytesIO(b"x"))
        try:
            extract_archive(malicious_tar, root / "tar-output")
        except ValueError as error:
            assert "unsafe archive member" in str(error)
        else:
            raise AssertionError("tar path traversal must be rejected")

        malicious_zip = root / "malicious.zip"
        with zipfile.ZipFile(malicious_zip, mode="w") as archive:
            archive.writestr("C:/escape", b"x")
        try:
            extract_archive(malicious_zip, root / "zip-output")
        except ValueError as error:
            assert "unsafe archive member" in str(error)
        else:
            raise AssertionError("zip absolute-drive path must be rejected")

        evidence = root / "evidence"
        evidence.mkdir()
        commit = "a" * 40
        create_evidence_fixture(evidence, commit)
        verified = subprocess.run(
            [
                sys.executable,
                str(verifier),
                "--evidence-dir",
                str(evidence),
                "--expected-commit",
                commit,
                "--expected-platform",
                "linux",
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        assert verified.returncode == 0, verified.stdout + verified.stderr
        with (evidence / "upgrade-v03-junit.xml").open("ab") as stream:
            stream.write(b"tamper")
        rejected = subprocess.run(
            [
                sys.executable,
                str(verifier),
                "--evidence-dir",
                str(evidence),
                "--expected-commit",
                commit,
                "--expected-platform",
                "linux",
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        assert rejected.returncode != 0, "tampered consumer evidence must be rejected"

    completed = subprocess.run(
        [sys.executable, "-m", "py_compile", str(tool), str(verifier)],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr
    print("validated external release consumer wiring, pinned v0.2 identity, and archive safety")


if __name__ == "__main__":
    main()
