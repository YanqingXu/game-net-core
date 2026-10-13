#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Exercise HP0 observer readiness without producing promotion evidence.

Implements intents/usecases/hot_path_cost_baseline.intent.md and the native,
complete-cost evidence boundary in rules/testing_rules.md. Probe runs retain
the preregistered arguments but never count as ledger warmups or samples.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any

from hot_path_cost_common import (
    HotPathCostError,
    detect_host,
    load_inventory,
    load_observation,
    require,
    require_complete_costs,
    sha256_file,
    validate_child,
    validate_host_identity,
)
from run_hot_path_cost import (
    decode_stderr,
    git_output,
    merge_costs,
    prepare_output,
    run_child,
)


def observer_handshake(
    observer_command: list[str], root: Path, timeout_seconds: int,
) -> list[dict[str, Any]]:
    """Run real children to check forwarding; a capability declaration cannot pass."""
    records = []
    for name, child_exit in (("success", 0), ("failure", 37)):
        nonce = uuid.uuid4().hex
        expected = (json.dumps({"nonce": nonce, "probe": name, "marker": "\u6d4b\u8bd5"}, ensure_ascii=False) + "\n").encode("utf-8")
        observation = root / f"handshake-{name}.observation.json"
        require(not observation.exists(), "handshake observation must be fresh")
        child = [
            sys.executable, "-c",
            "import sys; sys.stdout.buffer.write(bytes.fromhex(sys.argv[1])); "
            "sys.stdout.buffer.flush(); sys.exit(int(sys.argv[2]))",
            expected.hex(), str(child_exit),
        ]
        completed = subprocess.run(
            [*observer_command, "--cost-output", str(observation), "--", *child],
            capture_output=True, timeout=timeout_seconds, check=False,
        )
        stdout_path = root / f"handshake-{name}.stdout.bin"
        stderr_path = root / f"handshake-{name}.stderr.bin"
        stdout_path.write_bytes(completed.stdout)
        stderr_path.write_bytes(completed.stderr)
        require(completed.stdout == expected, f"observer {name} handshake did not preserve exact child stdout")
        if child_exit == 0:
            require(completed.returncode == 0, f"observer success handshake failed ({completed.returncode}): {decode_stderr(completed.stderr)}")
            load_observation(observation)
        else:
            require(completed.returncode != 0, "observer concealed a failing child's exit status")
        records.append({
            "probe": name,
            "nonce": nonce,
            "child_exit": child_exit,
            "observer_exit": completed.returncode,
            "stdout_path": stdout_path.name,
            "stdout_sha256": sha256_file(stdout_path),
            "stderr_path": stderr_path.name,
            "stderr_sha256": sha256_file(stderr_path),
        })
    return records


def probe_scenario(
    scenario: dict[str, Any], executable: Path, observer: Path,
    root: Path, platform: str, backend: str, timeout_seconds: int,
) -> dict[str, Any]:
    label = f"{scenario['key']} preflight"
    observation_path = root / f"{scenario['key']}.observation.json"
    document, stderr, observed = run_child(
        [str(executable), *scenario["arguments"]], observer, observation_path,
        timeout_seconds, label,
    )
    validated = validate_child(document, scenario, platform, backend, "Release", label)
    raw_path = root / f"{scenario['key']}.child.json"
    stderr_path = root / f"{scenario['key']}.stderr.bin"
    raw_path.write_text(json.dumps(validated, indent=2) + "\n", encoding="utf-8")
    stderr_path.write_bytes(stderr)
    costs, unavailable = merge_costs(validated, scenario, observed, label)
    require_complete_costs(costs, scenario, label)
    return {
        "key": scenario["key"], "arguments": scenario["arguments"],
        "costs": costs, "unavailable": unavailable,
        "raw_path": raw_path.name, "raw_sha256": sha256_file(raw_path),
        "stderr_path": stderr_path.name, "stderr_sha256": sha256_file(stderr_path),
        "observation_path": observation_path.name,
        "observation_sha256": sha256_file(observation_path),
    }


def run_preflight(args: argparse.Namespace) -> dict[str, Any]:
    inventory = load_inventory(args.inventory.resolve())
    report: dict[str, Any] = {
        "schema": "gamenet.hot_path_preflight.v1",
        "status": "blocked",
        "promotion_eligible": False,
        "observed_host": detect_host(),
        "inventory_sha256": sha256_file(args.inventory.resolve()),
        "runner_id": args.runner_id,
        "frequency_policy": args.frequency_policy,
        "observer_required_costs": {
            scenario["key"]: [
                key for key in scenario["required_costs"]
                if key not in scenario["cost_sources"]
            ] for scenario in inventory["scenarios"]
        },
        "blockers": [], "handshake": [], "probes": [],
    }
    blockers = report["blockers"]
    try:
        validate_host_identity(args.platform, args.backend, True, False)
    except HotPathCostError as error:
        blockers.append(str(error))
    source_root = args.source_root.resolve()
    try:
        commit = git_output(source_root, "rev-parse", "HEAD")
        report["commit_sha"] = commit
        if args.commit_sha is not None and commit != args.commit_sha:
            blockers.append("declared commit does not match source-root HEAD")
        status = git_output(source_root, "status", "--porcelain=v1", "--untracked-files=normal")
        report["working_tree"] = "dirty" if status else "clean"
        if status:
            blockers.append("fixed-lab preparation requires a clean exact checkout")
    except (HotPathCostError, OSError, subprocess.SubprocessError) as error:
        blockers.append(str(error))
    for value, label in ((args.runner_id, "runner id"), (args.frequency_policy, "reviewed frequency policy")):
        if not value.strip():
            blockers.append(f"{label} is missing")
    observer = args.observer_command
    if observer is None or not observer.is_absolute() or not observer.is_file():
        blockers.append("reviewed observer is missing or its path is not absolute")
    else:
        observer = observer.resolve()
        report["observer"] = {"path": str(observer), "sha256": sha256_file(observer)}
    executables: dict[str, Path] = {}
    report["executables"] = {}
    for group in sorted({scenario["executable"] for scenario in inventory["scenarios"]}):
        executable = getattr(args, f"{group}_executable")
        if executable is None or not executable.is_file():
            blockers.append(f"{group} Release executable is missing")
            continue
        executable = executable.resolve()
        executables[group] = executable
        report["executables"][group] = {"path": str(executable), "sha256": sha256_file(executable)}
    if not args.probe:
        blockers.append("observer and scenario probes were not requested; use --probe after providing lab resources")
    if blockers:
        return report

    # Each scenario runs once with its exact arguments. These probes cannot
    # substitute for the independent warmup and ten-sample fixed-lab ledger.
    try:
        require(args.process_timeout_seconds > 0, "process timeout must be positive")
        report["handshake"] = observer_handshake([str(observer)], args.output_root, args.process_timeout_seconds)
        for scenario in inventory["scenarios"]:
            report["probes"].append(probe_scenario(
                scenario, executables[scenario["executable"]], observer,
                args.output_root, args.platform, args.backend, args.process_timeout_seconds,
            ))
        require(sha256_file(observer) == report["observer"]["sha256"], "observer changed during preflight")
        for group, executable in executables.items():
            require(sha256_file(executable) == report["executables"][group]["sha256"], f"{group} executable changed during preflight")
        require(git_output(source_root, "rev-parse", "HEAD") == report["commit_sha"], "source HEAD changed during preflight")
        require(not git_output(source_root, "status", "--porcelain=v1", "--untracked-files=normal"), "working tree changed during preflight")
    except (HotPathCostError, OSError, subprocess.SubprocessError) as error:
        blockers.append(str(error))
        return report
    report["status"] = "ready"
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Check HP0 native lab and observer readiness; produces no promotion evidence")
    parser.add_argument("--inventory", type=Path, default=Path("benchmarks/hot_path_cost_inventory.json"))
    parser.add_argument("--source-root", type=Path, default=Path("."))
    parser.add_argument("--commit-sha")
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--runner-id", default="")
    parser.add_argument("--frequency-policy", default="")
    parser.add_argument("--observer-command", type=Path)
    for group in ("core", "phase4", "capacity", "queued"):
        parser.add_argument(f"--{group}-executable", type=Path)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--probe", action="store_true", help="Execute observer handshake and one exact-argument probe per scenario")
    parser.add_argument("--process-timeout-seconds", type=int, default=180)
    args = parser.parse_args()
    try:
        prepare_output(args.output_root)
        report = run_preflight(args)
        report_path = args.output_root / "preflight.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (HotPathCostError, OSError, subprocess.SubprocessError) as error:
        print(f"hot-path preflight failed: {error}", file=sys.stderr)
        return 1
    print(f"hot-path preflight {report['status']}: {report_path}; promotion_eligible=false")
    for blocker in report["blockers"]:
        print(f"  - {blocker}")
    return 0 if report["status"] == "ready" else 1


if __name__ == "__main__":
    raise SystemExit(main())
