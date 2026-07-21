from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path


BASELINE_SHA = "2b1be4343f7c478eb40542451f30aad8ca474003"
CANDIDATE_SHA = "a" * 40


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_matrix(root: Path, sha: str, runner, budgets, scale: float = 1.0) -> None:
    root.mkdir()
    scenarios = []
    budget_by_key = {item["key"]: item for item in budgets["scenarios"]}
    for scenario in runner.SCENARIOS:
        key = f"{scenario.group}.{scenario.key}"
        parameters = {"fixture_key": key}
        samples = []
        for repetition in range(1, budgets["repetitions"] + 1):
            measurements = {
                metric["name"]: 100.0 * scale
                for metric in budget_by_key[key]["metrics"]
            }
            document = {
                "schema": scenario.schema,
                "status": "ok",
                "error": None,
                "scenario": scenario.reported_scenario,
                "platform": "windows",
                "backend": "iocp",
                "build_type": "Release",
                "parameters": parameters,
                "measurements": measurements,
            }
            relative = Path(scenario.group) / f"{scenario.key}-{repetition}.json"
            path = root / relative
            path.parent.mkdir(exist_ok=True)
            path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            samples.append({"path": relative.as_posix(), "sha256": sha256_file(path)})
        scenarios.append({
            "key": key,
            "group": scenario.group,
            "reported_scenario": scenario.reported_scenario,
            "schema": scenario.schema,
            "parameters": parameters,
            "samples": samples,
        })
    manifest = {
        "schema": runner.SCHEMA,
        "commit_sha": sha,
        "platform": "windows",
        "backend": "iocp",
        "build_type": "Release",
        "repetitions": budgets["repetitions"],
        "executables": {
            "core": {"path": "fixture-core", "sha256": "1" * 64},
            "phase4": {"path": "fixture-phase4", "sha256": "2" * 64},
        },
        "scenarios": scenarios,
    }
    (root / "matrix-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    runner_path = repo_root / "tools" / "run_performance_matrix.py"
    comparator = repo_root / "tools" / "compare_performance_regression.py"
    budget_path = repo_root / "benchmarks" / "performance_regression_budgets.json"
    workflow = repo_root / ".github" / "workflows" / "core-benchmark.yml"
    runner = load_module(runner_path, "gamenet_performance_matrix_runner")
    budgets = json.loads(budget_path.read_text(encoding="utf-8"))

    assert budgets["schema"] == "gamenet.performance_regression_budget.v1"
    assert budgets["baseline_sha"] == BASELINE_SHA
    assert budgets["repetitions"] == 3
    runner_keys = {f"{scenario.group}.{scenario.key}" for scenario in runner.SCENARIOS}
    budget_keys = {scenario["key"] for scenario in budgets["scenarios"]}
    assert runner_keys == budget_keys
    assert len(runner_keys) == 12

    with tempfile.TemporaryDirectory(prefix="gamenet-performance-regression-") as directory:
        root = Path(directory)
        baseline = root / "baseline"
        candidate = root / "candidate"
        output = root / "regression.json"
        write_matrix(baseline, BASELINE_SHA, runner, budgets)
        write_matrix(candidate, CANDIDATE_SHA, runner, budgets)
        command = [
            sys.executable,
            str(comparator),
            "--baseline-root", str(baseline),
            "--candidate-root", str(candidate),
            "--budget", str(budget_path),
            "--baseline-sha", BASELINE_SHA,
            "--candidate-sha", CANDIDATE_SHA,
            "--platform", "windows",
            "--backend", "iocp",
            "--output", str(output),
        ]
        positive = subprocess.run(command, capture_output=True, text=True, check=False)
        assert positive.returncode == 0, positive.stderr
        evidence = json.loads(output.read_text(encoding="utf-8"))
        assert evidence["schema"] == "gamenet.performance_regression.v1"
        assert evidence["result"] == "pass"
        assert len(evidence["comparisons"]) == 12
        assert all(len(metric["candidate_samples"]) == 3
                   for scenario in evidence["comparisons"] for metric in scenario["metrics"])

        first_budget = budgets["scenarios"][0]
        first_metric = first_budget["metrics"][0]
        failing_scale = 10.0 if first_metric["direction"] == "lower" else 0.1
        candidate_failure = root / "candidate-failure"
        write_matrix(candidate_failure, CANDIDATE_SHA, runner, budgets, scale=failing_scale)
        failing_command = list(command)
        failing_command[failing_command.index(str(candidate))] = str(candidate_failure)
        negative = subprocess.run(failing_command, capture_output=True, text=True, check=False)
        assert negative.returncode == 1, negative.stderr
        failed = json.loads(output.read_text(encoding="utf-8"))
        assert failed["result"] == "fail"
        assert any(item["result"] == "fail" for item in failed["comparisons"])

    workflow_text = workflow.read_text(encoding="utf-8")
    assert workflow_text.count("Checkout performance baseline") == 2
    assert workflow_text.count(BASELINE_SHA) >= 2
    assert workflow_text.count("tools/run_performance_matrix.py") >= 4
    assert workflow_text.count("tools/compare_performance_regression.py") >= 2
    assert workflow_text.count("performance-regression.json") >= 2

    print("performance regression matrix and failure fixtures verified")


if __name__ == "__main__":
    main()
