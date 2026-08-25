# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing HP0 hot-path contract fragment in {source}: {needle}"


def main() -> None:
    repo = Path(__file__).resolve().parents[2]
    top_cmake = repo / "CMakeLists.txt"
    benchmark_cmake = repo / "benchmarks" / "CMakeLists.txt"
    inventory_path = repo / "benchmarks" / "hot_path_cost_inventory.json"
    intent_index = repo / "intents" / "README.md"
    intent = repo / "intents" / "usecases" / "hot_path_cost_baseline.intent.md"
    rules = repo / "rules" / "testing_rules.md"
    runner = repo / "tools" / "run_hot_path_cost.py"
    common = repo / "tools" / "hot_path_cost_common.py"
    validator = repo / "tools" / "validate_hot_path_cost.py"
    docs = repo / "docs" / "development" / "hot_path_cost_lab.md"
    workflow = repo / ".github" / "workflows" / "hot-path-benchmark.yml"
    gitignore = repo / ".gitignore"

    top_text = top_cmake.read_text(encoding="utf-8")
    cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    intent_text = intent.read_text(encoding="utf-8")
    rules_text = rules.read_text(encoding="utf-8")
    runner_text = runner.read_text(encoding="utf-8")
    common_text = common.read_text(encoding="utf-8")
    validator_text = validator.read_text(encoding="utf-8")
    docs_text = docs.read_text(encoding="utf-8")

    require(top_text, 'option(GAMENET_BUILD_BENCHMARKS "Build opt-in performance benchmarks" OFF)', top_cmake)
    require(top_text, "if(GAMENET_BUILD_BENCHMARKS)", top_cmake)
    require(cmake_text, "add_custom_target(gamenet_hot_path_benchmark", benchmark_cmake)
    for dependency in (
        "gamenet_core_benchmark",
        "gamenet_phase4_benchmark",
        "gamenet_capacity_profile",
        "gamenet_multi_io_queued_benchmark",
    ):
        require(cmake_text, dependency, benchmark_cmake)
    assert "add_test(" not in cmake_text, "hot-path suite must not be a CTest"
    assert "install(" not in cmake_text, "hot-path suite must not be installed"

    require(intent_index.read_text(encoding="utf-8"), "intents/usecases/hot_path_cost_baseline.intent.md", intent_index)
    for fragment in (
        "status: active",
        "target: gamenet_hot_path_benchmark",
        "migration_source: native",
        "artifact_kind: benchmark",
        "gamenet.hot_path_cost.v1",
        "callback and mutex-backed",
        "strict UTF-8",
        "public runtime",
        "HP1 cannot start",
        "tests/cmake/test_hot_path_benchmark_contract.py",
    ):
        require(intent_text, fragment, intent)
    for fragment in (
        "one unrecorded warmup",
        "least ten recorded samples",
        "exact clean",
        "synthetic zero",
        "native Linux/epoll",
        "native Windows/IOCP",
        "rejects WSL",
    ):
        require(rules_text, fragment, rules)

    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    assert inventory["schema"] == "gamenet.hot_path_cost_inventory.v1"
    assert [item["key"] for item in inventory["scenarios"]] == [
        "framing.callback-copy",
        "network-logic.callback-mutex",
        "readiness-send.callback-borrowed",
        "broadcast.current-owner-tasks",
        "connection-capacity.current",
        "overload-recovery.current",
    ]
    assert set().union(*(set(item["domains"]) for item in inventory["scenarios"])) >= {
        "framing",
        "network_logic",
        "readiness",
        "send",
        "broadcast",
        "connection_capacity",
        "overload_recovery",
        "shutdown",
    }
    assert all(item["primary"] and item["guardrails"] and item["required_costs"] for item in inventory["scenarios"])

    sys.path.insert(0, str(repo / "tools"))
    from hot_path_cost_common import (  # noqa: PLC0415
        COST_KEYS,
        HotPathCostError,
        load_inventory,
        load_observation,
    )

    loaded = load_inventory(inventory_path)
    assert tuple(loaded["cost_keys"]) == COST_KEYS
    with tempfile.TemporaryDirectory(prefix="gamenet-hot-path-observer-") as temporary:
        observation = Path(temporary) / "observation.json"
        observation.write_text(
            json.dumps({
                "schema": "gamenet.hot_path_observation.v1",
                "costs": {key: (0 if key == "cycles" else None) for key in COST_KEYS},
            }),
            encoding="utf-8",
        )
        parsed = load_observation(observation)
        assert parsed["cycles"] == 0
        assert parsed["instructions"] is None
        invalid = json.loads(observation.read_text(encoding="utf-8"))
        invalid["costs"]["cycles"] = -1
        observation.write_text(json.dumps(invalid), encoding="utf-8")
        try:
            load_observation(observation)
        except HotPathCostError as error:
            assert "must be non-negative" in str(error)
        else:
            raise AssertionError("negative observer costs must fail closed")

    for source, fragments in (
        (common, ("LEDGER_SCHEMA", "COST_KEYS", "validate_child", "HotPathCostError")),
        (runner, ("scenario-round-robin", "rev-parse", "--porcelain=v1", "observer-command", "missing required costs", "backslashreplace")),
        (validator, ("--require-fixed-lab", "raw hash mismatch", "promotion eligibility", "parameters drifted")),
        (docs, ("Observer contract", "development-only", "self-hosted", "95% bootstrap")),
    ):
        text = source.read_text(encoding="utf-8")
        for fragment in fragments:
            require(text, fragment, source)

    for script in (runner, validator):
        result = subprocess.run(
            [sys.executable, str(script), "--help"],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, result.stderr

    invalid_manifest = subprocess.run(
        [
            sys.executable,
            str(validator),
            "--input",
            str(inventory_path),
        ],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
    )
    assert invalid_manifest.returncode != 0
    assert "ledger schema mismatch" in invalid_manifest.stderr
    assert workflow.is_file(), f"missing manual fixed-lab workflow: {workflow}"
    require(gitignore.read_text(encoding="utf-8"), "hot-path-results/", gitignore)


if __name__ == "__main__":
    main()
