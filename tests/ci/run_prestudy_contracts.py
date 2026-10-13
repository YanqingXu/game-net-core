# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Run isolated HP1-HP6 correctness contracts; never performance promotion."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


CONTRACTS = {
    "contract.protocol.test_packet_framer_view": "gamenet_hp1_packet_framer_view_contract",
    "contract.runtime_model.test_spsc_mailbox": "gamenet_hp2_spsc_mailbox_contract",
    "contract.event_loop.test_event_loop_mailbox_source": "gamenet_hp2_mailbox_source_contract",
    "contract.io_engine.test_epoll_slot_dispatch": "gamenet_hp3_epoll_slot_dispatch_contract",
    "contract.tcp_connection.test_output_segment_chain": "gamenet_hp4_output_segment_chain_contract",
    "contract.tcp_connection.test_credit_lease": "gamenet_hp5_credit_lease_contract",
    "contract.event_loop.test_adaptive_phase_scheduler": "gamenet_hp6_adaptive_scheduler_contract",
}


def verify_inventory(document: dict[str, Any]) -> None:
    tests = document.get("tests", [])
    names = [item["name"] for item in tests]
    if len(names) != len(CONTRACTS) or set(names) != set(CONTRACTS):
        raise ValueError(f"prestudy inventory drift: {names}")
    for test in tests:
        properties = {item["name"]: item["value"] for item in test.get("properties", [])}
        labels = properties.get("LABELS", [])
        if not {"contract", "experimental", "prestudy"}.issubset(labels):
            raise ValueError(f"missing prestudy labels: {test['name']}")
        if properties.get("DISABLED", False):
            raise ValueError(f"disabled prestudy contract: {test['name']}")


def verify_results(document: ET.Element) -> None:
    tests = document.findall(".//testcase")
    names = [test.get("name") for test in tests]
    if len(names) != len(CONTRACTS) or set(names) != set(CONTRACTS):
        raise ValueError(f"prestudy execution drift: {names}")
    for test in tests:
        if test.get("status") != "run" or any(
            test.find(tag) is not None for tag in ("failure", "error", "skipped")
        ):
            raise ValueError(f"prestudy contract did not pass: {test.get('name')}")


def configure_command(repo: Path, build: Path, profile: str, windows: bool) -> list[str]:
    if windows and profile == "tsan":
        raise ValueError("MSVC does not support TSan")
    configuration = "Release" if profile == "release" else "Debug"
    command = ["cmake", "-S", str(repo), "-B", str(build)]
    if windows:
        command.extend(["-A", "x64"])
    command.extend([
        f"-DCMAKE_BUILD_TYPE={configuration}",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DGAMENET_BUILD_TESTING=ON",
        "-DGAMENET_BUILD_BENCHMARKS=ON",
        "-DGAMENET_BUILD_FUZZING=OFF",
        "-DGAMENET_ENABLE_EXPERIMENTAL=OFF",
        "-DGAMENET_ENABLE_TLS=OFF",
        f"-DGAMENET_ENABLE_ASAN_UBSAN={'ON' if profile == 'asan' else 'OFF'}",
        f"-DGAMENET_ENABLE_TSAN={'ON' if profile == 'tsan' else 'OFF'}",
    ])
    return command


def run(arguments: argparse.Namespace) -> None:
    repo = Path(__file__).resolve().parents[2]
    build = arguments.build_dir.resolve()
    output = arguments.output.resolve()
    for directory in (build, output):
        if directory.exists() and any(directory.iterdir()):
            raise ValueError(f"use a new or empty directory: {directory}")
    output.mkdir(parents=True, exist_ok=True)

    def execute(name: str, command: list[str]) -> str:
        (output / f"{name}-command.json").write_text(
            json.dumps(command, indent=2) + "\n", encoding="utf-8"
        )
        print(f"Running {name}", flush=True)
        environment = os.environ.copy()
        # Sanitizer diagnostics must fail a contract, including recoverable UBSan.
        if arguments.profile == "asan":
            environment["ASAN_OPTIONS"] = "halt_on_error=1"
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        elif arguments.profile == "tsan":
            environment["TSAN_OPTIONS"] = "halt_on_error=1"
        with (output / f"{name}.log").open("wb") as log:
            result = subprocess.run(
                command, cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                check=False, timeout=1800, env=environment,
            )
        text = (output / f"{name}.log").read_text(encoding="utf-8", errors="replace")
        print(text, end="", flush=True)
        if result.returncode:
            raise RuntimeError(f"{name} failed with exit code {result.returncode}")
        return text

    # Record an attempted run before configuration, including failed runs.
    manifest: dict[str, Any] = {
        "schema": "gamenet.prestudy_contracts.v1",
        "evidence_class": "development-contracts",
        "promotion_eligible": False,
        "profile": arguments.profile,
        "platform": platform.platform(),
        "workflow_sha": os.environ.get("GITHUB_SHA"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "contracts": sorted(CONTRACTS),
        "status": "failed",
    }
    try:
        manifest["commit_sha"] = execute("commit", ["git", "rev-parse", "HEAD"]).strip()
        manifest["working_tree"] = execute("worktree", ["git", "status", "--porcelain=v1"])
        expected_sha = manifest["workflow_sha"]
        if expected_sha and expected_sha != manifest["commit_sha"]:
            raise ValueError("checkout SHA does not match workflow SHA")
        execute("cmake-version", ["cmake", "--version"])
        execute("ctest-version", ["ctest", "--version"])
        configuration = "Release" if arguments.profile == "release" else "Debug"
        execute("configure", configure_command(repo, build, arguments.profile, os.name == "nt"))
        inventory_command = [
            "ctest", "--test-dir", str(build), "-C", configuration,
            "-L", "^prestudy$", "--show-only=json-v1",
        ]
        # Verify selected names before a build can silently omit a contract.
        inventory = execute("inventory", inventory_command)
        verify_inventory(json.loads(inventory))
        execute("build", [
            "cmake", "--build", str(build), "--config", configuration,
            "--parallel", "2", "--target", *CONTRACTS.values(),
        ])
        junit = output / "ctest-junit.xml"
        execute("ctest", [
            "ctest", "--test-dir", str(build), "-C", configuration,
            "-L", "^prestudy$", "--parallel", "1", "--timeout", "60",
            "--no-tests=error", "--output-on-failure", "--output-junit", str(junit),
        ])
        verify_results(ET.parse(junit).getroot())
        manifest["status"] = "passed"
    finally:
        (output / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, choices=("release", "asan", "tsan"))
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        run(arguments)
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired, ET.ParseError) as error:
        print(f"Prestudy contract run failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
