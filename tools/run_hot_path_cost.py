#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hot_path_cost_common import (  # noqa: E402
    COST_KEYS,
    LEDGER_SCHEMA,
    HotPathCostError,
    child_parameters,
    extract_scaled,
    load_inventory,
    load_observation,
    metric_record,
    require,
    sha256_file,
    validate_child,
)


def prepare_output(path: Path) -> None:
    if path.exists():
        require(path.is_dir(), f"output root is not a directory: {path}")
        require(not any(path.iterdir()), f"output root must be empty: {path}")
    else:
        path.mkdir(parents=True)


def decode_stdout(data: bytes, label: str) -> str:
    try:
        return data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise HotPathCostError(f"{label}: stdout is not strict UTF-8: {error}") from error


def decode_stderr(data: bytes) -> str:
    return data.decode("utf-8", errors="backslashreplace")


def git_output(source_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=source_root,
        capture_output=True,
        timeout=30,
        check=False,
    )
    stderr = decode_stderr(completed.stderr)
    require(
        completed.returncode == 0,
        f"git {' '.join(arguments)} failed: {stderr.strip() or '<empty>'}",
    )
    return decode_stdout(completed.stdout, "git output").strip()


def run_child(
    command: list[str],
    observer: Path | None,
    observation_path: Path,
    timeout_seconds: int,
    label: str,
) -> tuple[dict[str, Any], bytes, dict[str, int | float | None] | None]:
    actual = command
    if observer is not None:
        actual = [str(observer), "--cost-output", str(observation_path), "--", *command]
    completed = subprocess.run(
        actual,
        capture_output=True,
        timeout=timeout_seconds,
        check=False,
    )
    stderr_text = decode_stderr(completed.stderr)
    require(completed.returncode == 0, f"{label}: child failed ({completed.returncode}): {stderr_text.strip() or '<empty>'}")
    stdout_text = decode_stdout(completed.stdout, label)
    try:
        document = json.loads(stdout_text.lstrip("\ufeff"))
    except json.JSONDecodeError as error:
        raise HotPathCostError(f"{label}: child stdout is not one JSON document: {error}") from error
    observation = None
    if observer is not None:
        require(observation_path.is_file(), f"{label}: observer did not create {observation_path}")
        observation = load_observation(observation_path)
    return document, completed.stderr, observation


def merge_costs(
    document: dict[str, Any],
    scenario: dict[str, Any],
    observed: dict[str, int | float | None] | None,
    label: str,
) -> tuple[dict[str, int | float | None], dict[str, str]]:
    values: dict[str, int | float | None] = {key: None for key in COST_KEYS}
    for key, source in scenario["cost_sources"].items():
        values[key] = extract_scaled(document, source, f"{label}: cost {key}")
    if observed is not None:
        for key, observed_value in observed.items():
            if observed_value is None:
                continue
            if values[key] is not None:
                require(values[key] == observed_value, f"{label}: observer/raw cost conflict for {key}")
            values[key] = observed_value
    unavailable = {
        key: "not exposed by the child benchmark or configured observer"
        for key, value in values.items()
        if value is None
    }
    return values, unavailable


def run_suite(args: argparse.Namespace) -> dict[str, Any]:
    source_root = args.source_root.resolve()
    require((source_root / ".git").exists(), f"source root is not a Git worktree: {source_root}")
    require(re.fullmatch(r"[0-9a-f]{40}", args.commit_sha) is not None, "commit SHA must be 40 lowercase hex digits")
    actual_commit = git_output(source_root, "rev-parse", "HEAD")
    require(actual_commit == args.commit_sha, "declared commit does not match source-root HEAD")
    status_output = git_output(
        source_root,
        "status",
        "--porcelain=v1",
        "--untracked-files=normal",
    )
    actual_working_tree = "clean" if not status_output else "dirty"
    require(
        actual_working_tree == args.working_tree,
        "declared working-tree state does not match Git status",
    )
    require(args.backend == ("epoll" if args.platform == "linux" else "iocp"), "platform/backend pair mismatch")
    require(args.warmups == 1, "HP0 requires exactly one unrecorded warmup")
    require(args.samples >= 1, "sample count must be positive")
    if args.evidence_class == "fixed-lab":
        require(args.samples >= 10, "fixed-lab evidence requires at least ten samples")
        require(args.working_tree == "clean", "fixed-lab evidence requires a clean working tree")
        require(args.native_runner, "fixed-lab evidence requires a native runner")
        require(not args.wsl, "WSL cannot produce fixed-lab evidence")
        require(args.build_type == "Release", "fixed-lab evidence requires Release")
        require(args.observer_command is not None, "fixed-lab evidence requires a cost observer")
    inventory = load_inventory(args.inventory.resolve())
    executable_arguments = {
        "core": args.core_executable,
        "phase4": args.phase4_executable,
        "capacity": args.capacity_executable,
        "queued": args.queued_executable,
    }
    executables: dict[str, Path] = {}
    for group in {scenario["executable"] for scenario in inventory["scenarios"]}:
        path = executable_arguments[group].resolve()
        require(path.is_file(), f"{group} executable missing: {path}")
        executables[group] = path
    observer = args.observer_command.resolve() if args.observer_command else None
    if observer is not None:
        require(observer.is_file(), f"cost observer missing: {observer}")
    prepare_output(args.output_root)
    raw_root = args.output_root / "raw"
    raw_root.mkdir()
    records: dict[str, dict[str, Any]] = {}
    scenario_by_key = {item["key"]: item for item in inventory["scenarios"]}
    for scenario in inventory["scenarios"]:
        scenario_root = raw_root / scenario["key"]
        scenario_root.mkdir(parents=True)
        records[scenario["key"]] = {
            "key": scenario["key"],
            "domains": scenario["domains"],
            "baseline": scenario["baseline"],
            "hp_slices": scenario["hp_slices"],
            "executable": scenario["executable"],
            "child_schema": scenario["child_schema"],
            "arguments": scenario["arguments"],
            "primary": {key: scenario["primary"][key] for key in ("name", "direction")},
            "guardrails": [{key: item[key] for key in ("name", "direction")} for item in scenario["guardrails"]],
            "required_costs": scenario["required_costs"],
            "child_parameters": None,
            "samples": [],
        }

    # Warm every scenario once. These runs are deliberately not entered in the
    # ledger sample set and cannot satisfy the recorded-sample minimum.
    for scenario in inventory["scenarios"]:
        command = [str(executables[scenario["executable"]]), *scenario["arguments"]]
        document, _, _ = run_child(
            command,
            observer,
            args.output_root / "warmup-observation.json",
            args.process_timeout_seconds,
            f"{scenario['key']} warmup",
        )
        validate_child(document, scenario, args.platform, args.backend, args.build_type, f"{scenario['key']} warmup")
        warmup_observation = args.output_root / "warmup-observation.json"
        if warmup_observation.exists():
            warmup_observation.unlink()

    # Scenario round-robin avoids running all samples from one subsystem in a
    # single temperature/time block.
    for ordinal in range(1, args.samples + 1):
        for key in scenario_by_key:
            scenario = scenario_by_key[key]
            record = records[key]
            scenario_root = raw_root / key
            raw_path = scenario_root / f"sample-{ordinal:02d}.json"
            stderr_path = scenario_root / f"sample-{ordinal:02d}.stderr.txt"
            observation_path = scenario_root / f"sample-{ordinal:02d}.observation.json"
            command = [str(executables[scenario["executable"]]), *scenario["arguments"]]
            document, stderr, observed = run_child(
                command,
                observer,
                observation_path,
                args.process_timeout_seconds,
                f"{key} sample {ordinal}",
            )
            validated = validate_child(document, scenario, args.platform, args.backend, args.build_type, f"{key} sample {ordinal}")
            parameters = child_parameters(validated, scenario)
            if record["child_parameters"] is None:
                record["child_parameters"] = parameters
            else:
                require(record["child_parameters"] == parameters, f"{key}: child parameters drifted")
            raw_path.write_text(json.dumps(validated, indent=2) + "\n", encoding="utf-8")
            stderr_path.write_bytes(stderr)
            costs, unavailable = merge_costs(validated, scenario, observed, f"{key} sample {ordinal}")
            sample = {
                "ordinal": ordinal,
                "raw_path": raw_path.relative_to(args.output_root).as_posix(),
                "raw_sha256": sha256_file(raw_path),
                "stderr_path": stderr_path.relative_to(args.output_root).as_posix(),
                "stderr_sha256": sha256_file(stderr_path),
                "observation_path": observation_path.relative_to(args.output_root).as_posix() if observation_path.exists() else None,
                "observation_sha256": sha256_file(observation_path) if observation_path.exists() else None,
                "primary": metric_record(validated, scenario["primary"], f"{key}: primary"),
                "guardrails": [metric_record(validated, item, f"{key}: guardrail {item['name']}") for item in scenario["guardrails"]],
                "costs": costs,
                "unavailable": unavailable,
            }
            record["samples"].append(sample)

    missing_required: list[str] = []
    for scenario in inventory["scenarios"]:
        record = records[scenario["key"]]
        for sample in record["samples"]:
            for cost in scenario["required_costs"]:
                if sample["costs"][cost] is None:
                    missing_required.append(f"{scenario['key']}#{sample['ordinal']}:{cost}")
    fixed_environment = (
        args.evidence_class == "fixed-lab"
        and args.samples >= 10
        and args.working_tree == "clean"
        and args.native_runner
        and not args.wsl
        and args.build_type == "Release"
        and observer is not None
    )
    promotion_eligible = fixed_environment and not missing_required
    manifest = {
        "schema": LEDGER_SCHEMA,
        "status": "ok" if args.evidence_class == "development" or promotion_eligible else "incomplete",
        "evidence_class": args.evidence_class,
        "promotion_eligible": promotion_eligible,
        "identity": {
            "commit_sha": args.commit_sha,
            "working_tree": args.working_tree,
            "platform": args.platform,
            "backend": args.backend,
            "build_type": args.build_type,
            "native_runner": args.native_runner,
            "wsl": args.wsl,
            "runner_id": args.runner_id,
            "source_root": str(source_root),
        },
        "environment": {
            "cpu_model": args.cpu_model,
            "cpu_affinity": args.cpu_affinity,
            "frequency_policy": args.frequency_policy,
            "compiler": args.compiler,
            "command_context": args.command_context,
        },
        "sampling": {
            "warmups_per_scenario": args.warmups,
            "recorded_samples_per_scenario": args.samples,
            "order": "scenario-round-robin",
        },
        "inventory": {
            "path": str(args.inventory.resolve()),
            "sha256": sha256_file(args.inventory.resolve()),
        },
        "executables": {
            group: {"path": str(path), "sha256": sha256_file(path)}
            for group, path in sorted(executables.items())
        },
        "observer": None if observer is None else {"path": str(observer), "sha256": sha256_file(observer)},
        "cost_keys": list(COST_KEYS),
        "missing_required_costs": missing_required,
        "scenarios": [records[item["key"]] for item in inventory["scenarios"]],
    }
    manifest_path = args.output_root / "hot-path-cost.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if args.evidence_class == "fixed-lab" and not promotion_eligible:
        raise HotPathCostError(
            "fixed-lab evidence is incomplete; missing required costs: "
            + ", ".join(missing_required[:12])
            + (" ..." if len(missing_required) > 12 else "")
        )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the HP0 hot-path cost ledger")
    parser.add_argument("--inventory", type=Path, default=Path("benchmarks/hot_path_cost_inventory.json"))
    parser.add_argument("--source-root", type=Path, default=Path("."))
    parser.add_argument("--core-executable", type=Path, required=True)
    parser.add_argument("--phase4-executable", type=Path, required=True)
    parser.add_argument("--capacity-executable", type=Path, required=True)
    parser.add_argument("--queued-executable", type=Path, required=True)
    parser.add_argument("--observer-command", type=Path)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--working-tree", choices=("clean", "dirty"), required=True)
    parser.add_argument("--evidence-class", choices=("development", "fixed-lab"), default="development")
    parser.add_argument("--native-runner", action="store_true")
    parser.add_argument("--wsl", action="store_true")
    parser.add_argument("--runner-id", required=True)
    parser.add_argument("--cpu-model", required=True)
    parser.add_argument("--cpu-affinity", required=True)
    parser.add_argument("--frequency-policy", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--command-context", required=True)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--samples", type=int, default=10)
    parser.add_argument("--process-timeout-seconds", type=int, default=180)
    args = parser.parse_args()
    try:
        manifest = run_suite(args)
    except (HotPathCostError, OSError, subprocess.SubprocessError) as error:
        print(f"hot-path cost run failed: {error}", file=sys.stderr)
        return 1
    print(
        "hot-path cost run completed: "
        f"{len(manifest['scenarios'])} scenarios x {args.samples} samples; "
        f"promotion_eligible={str(manifest['promotion_eligible']).lower()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
