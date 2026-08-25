#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import json
import math
import re
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


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise HotPathCostError(f"cannot read {label} {path}: {error}") from error
    require(isinstance(document, dict), f"{label} must be an object")
    return document


def nonempty_string(value: Any, label: str) -> str:
    require(isinstance(value, str) and value.strip(), f"{label} must be a non-empty string")
    return value


def artifact_path(root: Path, relative: Any, label: str) -> Path:
    require(isinstance(relative, str) and relative, f"{label} path missing")
    candidate = Path(relative)
    require(not candidate.is_absolute(), f"{label} path must be relative")
    resolved_root = root.resolve()
    resolved = (resolved_root / candidate).resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as error:
        raise HotPathCostError(f"{label} path escapes the evidence root") from error
    return resolved


def validate_metric(actual: Any, expected: dict[str, Any], document: dict[str, Any], label: str) -> None:
    require(isinstance(actual, dict), f"{label} must be an object")
    rebuilt = metric_record(document, expected, label)
    require(actual == rebuilt, f"{label} does not match raw child evidence")


def validate_manifest(
    manifest_path: Path,
    inventory_path: Path,
    require_fixed_lab: bool,
) -> dict[str, Any]:
    manifest = load_json(manifest_path, "hot-path manifest")
    inventory = load_inventory(inventory_path)
    root = manifest_path.parent
    require(manifest.get("schema") == LEDGER_SCHEMA, "hot-path ledger schema mismatch")
    require(manifest.get("status") in {"ok", "incomplete"}, "hot-path ledger status invalid")
    require(manifest.get("evidence_class") in {"development", "fixed-lab"}, "hot-path evidence class invalid")
    require(isinstance(manifest.get("promotion_eligible"), bool), "promotion eligibility must be boolean")
    require(tuple(manifest.get("cost_keys", ())) == COST_KEYS, "hot-path ledger cost-key order drifted")

    identity = manifest.get("identity")
    environment = manifest.get("environment")
    sampling = manifest.get("sampling")
    require(isinstance(identity, dict), "hot-path identity missing")
    require(isinstance(environment, dict), "hot-path environment missing")
    require(isinstance(sampling, dict), "hot-path sampling missing")
    commit = identity.get("commit_sha")
    require(isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit) is not None, "invalid exact commit")
    platform = identity.get("platform")
    backend = identity.get("backend")
    build_type = identity.get("build_type")
    require(platform in {"linux", "windows"}, "invalid platform")
    require(backend == ("epoll" if platform == "linux" else "iocp"), "platform/backend mismatch")
    require(identity.get("working_tree") in {"clean", "dirty"}, "working-tree state missing")
    require(isinstance(identity.get("native_runner"), bool), "native-runner state missing")
    require(isinstance(identity.get("wsl"), bool), "WSL state missing")
    nonempty_string(identity.get("runner_id"), "runner id")
    nonempty_string(identity.get("source_root"), "source root")
    for field in ("cpu_model", "cpu_affinity", "frequency_policy", "compiler", "command_context"):
        nonempty_string(environment.get(field), f"environment {field}")
    require(sampling.get("warmups_per_scenario") == 1, "exactly one warmup is required")
    samples_per_scenario = sampling.get("recorded_samples_per_scenario")
    require(isinstance(samples_per_scenario, int) and samples_per_scenario > 0, "recorded sample count invalid")
    require(sampling.get("order") == "scenario-round-robin", "sampling order drifted")

    inventory_record = manifest.get("inventory")
    require(isinstance(inventory_record, dict), "inventory record missing")
    require(inventory_record.get("sha256") == sha256_file(inventory_path), "inventory SHA-256 mismatch")
    executables = manifest.get("executables")
    require(isinstance(executables, dict), "executable identities missing")
    expected_groups = {item["executable"] for item in inventory["scenarios"]}
    require(set(executables) == expected_groups, "executable identity set drifted")
    for group, executable in executables.items():
        require(isinstance(executable, dict), f"{group}: executable identity invalid")
        nonempty_string(executable.get("path"), f"{group}: executable path")
        require(isinstance(executable.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", executable["sha256"]) is not None, f"{group}: executable hash invalid")
    observer_record = manifest.get("observer")
    if observer_record is not None:
        require(isinstance(observer_record, dict), "observer identity must be null or an object")
        nonempty_string(observer_record.get("path"), "observer path")
        require(
            isinstance(observer_record.get("sha256"), str)
            and re.fullmatch(r"[0-9a-f]{64}", observer_record["sha256"]) is not None,
            "observer hash invalid",
        )

    scenarios = manifest.get("scenarios")
    require(isinstance(scenarios, list), "scenario records missing")
    require([item.get("key") for item in scenarios if isinstance(item, dict)] == [item["key"] for item in inventory["scenarios"]], "scenario inventory/order drifted")
    missing_required: list[str] = []
    for scenario, expected in zip(scenarios, inventory["scenarios"], strict=True):
        key = expected["key"]
        require(isinstance(scenario, dict), f"{key}: scenario record must be an object")
        for field in ("domains", "baseline", "hp_slices", "executable", "child_schema", "arguments", "required_costs"):
            require(scenario.get(field) == expected[field], f"{key}: {field} drifted")
        require(scenario.get("primary") == {name: expected["primary"][name] for name in ("name", "direction")}, f"{key}: primary preregistration drifted")
        require(scenario.get("guardrails") == [{name: item[name] for name in ("name", "direction")} for item in expected["guardrails"]], f"{key}: guardrail preregistration drifted")
        samples = scenario.get("samples")
        require(isinstance(samples, list) and len(samples) == samples_per_scenario, f"{key}: sample count mismatch")
        observed_parameters = None
        for ordinal, sample in enumerate(samples, 1):
            label = f"{key} sample {ordinal}"
            require(isinstance(sample, dict) and sample.get("ordinal") == ordinal, f"{label}: ordinal mismatch")
            raw_relative = sample.get("raw_path")
            stderr_relative = sample.get("stderr_path")
            raw_path = artifact_path(root, raw_relative, f"{label}: raw")
            stderr_path = artifact_path(root, stderr_relative, f"{label}: stderr")
            require(raw_path.is_file() and stderr_path.is_file(), f"{label}: raw artifact missing")
            require(sample.get("raw_sha256") == sha256_file(raw_path), f"{label}: raw hash mismatch")
            require(sample.get("stderr_sha256") == sha256_file(stderr_path), f"{label}: stderr hash mismatch")
            raw = load_json(raw_path, label)
            validated = validate_child(raw, expected, platform, backend, build_type, label)
            parameters = child_parameters(validated, expected)
            if observed_parameters is None:
                observed_parameters = parameters
            else:
                require(parameters == observed_parameters, f"{key}: parameters drifted between samples")
            validate_metric(sample.get("primary"), expected["primary"], validated, f"{label}: primary")
            guardrails = sample.get("guardrails")
            require(isinstance(guardrails, list) and len(guardrails) == len(expected["guardrails"]), f"{label}: guardrail count mismatch")
            for actual, specification in zip(guardrails, expected["guardrails"], strict=True):
                validate_metric(actual, specification, validated, f"{label}: guardrail {specification['name']}")
            observation_path = sample.get("observation_path")
            observation_hash = sample.get("observation_sha256")
            observed_costs = None
            if observation_path is None:
                require(observer_record is None, f"{label}: configured observer produced no artifact")
                require(observation_hash is None, f"{label}: observation hash without file")
            else:
                require(observer_record is not None, f"{label}: observation exists without observer identity")
                path = artifact_path(root, observation_path, f"{label}: observation")
                require(path.is_file(), f"{label}: observation artifact missing")
                require(observation_hash == sha256_file(path), f"{label}: observation hash mismatch")
                observed_costs = load_observation(path)
            rebuilt_costs: dict[str, int | float | None] = {
                name: None for name in COST_KEYS
            }
            for name, source in expected["cost_sources"].items():
                rebuilt_costs[name] = extract_scaled(
                    validated,
                    source,
                    f"{label}: cost {name}",
                )
            if observed_costs is not None:
                for name, value in observed_costs.items():
                    if value is None:
                        continue
                    if rebuilt_costs[name] is not None:
                        require(
                            rebuilt_costs[name] == value,
                            f"{label}: observer/raw cost conflict for {name}",
                        )
                    rebuilt_costs[name] = value
            costs = sample.get("costs")
            unavailable = sample.get("unavailable")
            require(isinstance(costs, dict) and tuple(costs) == COST_KEYS, f"{label}: cost-key order drifted")
            require(costs == rebuilt_costs, f"{label}: costs do not match raw/observer evidence")
            require(isinstance(unavailable, dict), f"{label}: unavailable reasons missing")
            null_costs = {name for name, value in costs.items() if value is None}
            require(set(unavailable) == null_costs, f"{label}: unavailable reason set mismatch")
            for name, value in costs.items():
                if value is None:
                    nonempty_string(unavailable[name], f"{label}: unavailable reason {name}")
                else:
                    require(isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value)) and value >= 0, f"{label}: invalid cost {name}")
            for name in expected["required_costs"]:
                if costs[name] is None:
                    missing_required.append(f"{key}#{ordinal}:{name}")
        require(scenario.get("child_parameters") == observed_parameters, f"{key}: recorded child parameters drifted")

    require(manifest.get("missing_required_costs") == missing_required, "missing-required-cost summary drifted")
    fixed_eligible = (
        manifest.get("evidence_class") == "fixed-lab"
        and identity.get("working_tree") == "clean"
        and identity.get("native_runner") is True
        and identity.get("wsl") is False
        and build_type == "Release"
        and samples_per_scenario >= 10
        and isinstance(observer_record, dict)
        and not missing_required
    )
    require(manifest.get("promotion_eligible") == fixed_eligible, "promotion eligibility is inconsistent")
    expected_status = "ok" if manifest.get("evidence_class") == "development" or fixed_eligible else "incomplete"
    require(manifest.get("status") == expected_status, "ledger status is inconsistent")
    if require_fixed_lab:
        require(fixed_eligible, "ledger is not complete fixed-lab evidence")
        require(manifest.get("status") == "ok", "fixed-lab ledger status is not ok")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate HP0 hot-path cost evidence")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, default=Path("benchmarks/hot_path_cost_inventory.json"))
    parser.add_argument("--require-fixed-lab", action="store_true")
    args = parser.parse_args()
    try:
        manifest = validate_manifest(args.input.resolve(), args.inventory.resolve(), args.require_fixed_lab)
    except HotPathCostError as error:
        print(f"hot-path cost validation failed: {error}", file=sys.stderr)
        return 1
    print(
        "hot-path cost validation passed: "
        f"{manifest['identity']['platform']}/{manifest['identity']['backend']} "
        f"{len(manifest['scenarios'])} scenarios"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
