from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace


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


def write_matrix(
    root: Path,
    sha: str,
    runner,
    budgets,
    scale: float = 1.0,
    *,
    legacy_core_v1: bool = False,
    legacy_parameter_drift: bool = False,
) -> None:
    root.mkdir()
    scenarios = []
    budget_by_key = {item["key"]: item for item in budgets["scenarios"]}
    for scenario in runner.SCENARIOS:
        key = f"{scenario.group}.{scenario.key}"
        parameters = {"fixture_key": key}
        schema = scenario.schema
        if scenario.group == "core":
            if legacy_core_v1:
                schema = "gamenet.core_benchmark.v1"
            else:
                parameters["v2_only_default"] = 1
                if legacy_parameter_drift:
                    parameters["fixture_key"] += "-drift"
        samples = []
        for repetition in range(1, budgets["repetitions"] + 1):
            measurements = {
                metric["name"]: 100.0 * scale
                for metric in budget_by_key[key]["metrics"]
            }
            document = {
                "schema": schema,
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
            "schema": schema,
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
    paired_runner = load_module(
        repo_root / "tools" / "run_paired_performance_matrix.py",
        "gamenet_paired_performance_matrix_runner",
    )
    budgets = json.loads(budget_path.read_text(encoding="utf-8"))

    assert budgets["schema"] == "gamenet.performance_regression_budget.v1"
    assert budgets["baseline_sha"] == BASELINE_SHA
    assert budgets["repetitions"] == 3
    runner_keys = {f"{scenario.group}.{scenario.key}" for scenario in runner.SCENARIOS}
    budget_keys = {scenario["key"] for scenario in budgets["scenarios"]}
    assert runner_keys == budget_keys
    assert len(runner_keys) == 12
    paired_source = (
        repo_root / "tools" / "run_paired_performance_matrix.py"
    ).read_text(encoding="utf-8")
    assert 'errors="backslashreplace"' in paired_source
    assert 'decode("utf-8", errors="strict")' in paired_source

    with tempfile.TemporaryDirectory(prefix="gamenet-paired-sampling-") as directory:
        root = Path(directory)
        candidate_executable = root / "candidate-core"
        baseline_executable = root / "baseline-core"
        candidate_executable.write_bytes(b"candidate")
        baseline_executable.write_bytes(b"baseline")
        fixture_scenario = runner.Scenario(
            "core",
            "fixture",
            "echo",
            "gamenet.core_benchmark.v2",
            ("--scenario", "echo"),
            True,
        )
        paired_runner.SCENARIO_PROFILES = {"regression": (fixture_scenario,)}
        calls: list[str] = []

        def fake_run(command, **_kwargs):
            identity = Path(command[0]).name.split("-")[0]
            calls.append(identity)
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=json.dumps(
                    {
                        "schema": "gamenet.core_benchmark.v2",
                        "status": "ok",
                        "error": None,
                        "scenario": "echo",
                        "platform": "windows",
                        "backend": "iocp",
                        "build_type": "Release",
                        "parameters": {"fixture": identity},
                        "measurements": {"fixture": 1.0},
                    }
                ),
                stderr="",
            )

        real_subprocess_run = subprocess.run
        paired_runner.subprocess.run = fake_run
        paired_runner.validate_document = (
            lambda document, *_args, **_kwargs: document
        )
        arguments = SimpleNamespace(
            repetitions=3,
            candidate_sha=CANDIDATE_SHA,
            baseline_sha=BASELINE_SHA,
            matrix_profile="regression",
            candidate_core_executable=candidate_executable,
            candidate_phase4_executable=None,
            baseline_core_executable=baseline_executable,
            baseline_phase4_executable=None,
            candidate_output_root=root / "candidate-output",
            baseline_output_root=root / "baseline-output",
            candidate_canonical_core_dir=root / "canonical-core",
            candidate_canonical_phase4_dir=None,
            platform="windows",
            backend="iocp",
            build_type="Release",
            process_timeout_seconds=10,
        )
        candidate_manifest, baseline_manifest = (
            paired_runner.run_paired_matrix(arguments)
        )
        assert calls == [
            "candidate",
            "baseline",
            "candidate",
            "baseline",
            "baseline",
            "candidate",
            "candidate",
            "baseline",
        ]
        assert candidate_manifest["sampling"] == {
            "schema": "gamenet.paired_interleaved.v1",
            "pair_role": "candidate",
            "peer_commit_sha": BASELINE_SHA,
            "warmups_per_scenario": 1,
            "order_rule": "scenario-and-repetition-parity",
        }
        assert baseline_manifest["sampling"]["pair_role"] == "baseline"
        assert len(candidate_manifest["scenarios"][0]["samples"]) == 3
        assert (root / "canonical-core" / "fixture.json").is_file()
        paired_runner.subprocess.run = real_subprocess_run

    slow_scenario = next(
        scenario for scenario in runner.SCENARIOS
        if scenario.group == "core" and scenario.key == "slow-client-4"
    )
    semantic_document = {
        "schema": "gamenet.core_benchmark.v2",
        "status": "ok",
        "error": None,
        "scenario": "slow-client",
        "platform": "windows",
        "backend": "iocp",
        "backpressure_policy": "bounded_output_hysteresis",
        "build_type": "Release",
        "parameters": {
            "connections": 2,
            "event_loop_threads": 1,
            "slow_bytes_per_connection": 33554432,
            "low_water_bytes": 32768,
            "high_water_bytes": 65536,
            "hard_limit_bytes": 16777216,
            "max_input_buffer_bytes": 2097152,
            "settle_ms": 1000,
        },
        "measurements": {
            "elapsed_seconds": 0.1,
            "round_trips": 0,
            "application_bytes": 0,
            "throughput_mib_per_second": None,
            "messages_per_second": None,
            "p50_latency_us": None,
            "p99_latency_us": None,
            "p999_latency_us": None,
            "working_set_before_bytes": 1000,
            "working_set_after_bytes": 1000,
            "working_set_delta_bytes": 0,
            "approx_bytes_per_connection": 0.0,
            "connection_establish_seconds": None,
            "connection_establish_per_second": None,
            "idle_observation_seconds": None,
            "idle_process_cpu_seconds": None,
            "idle_process_cpu_percent": None,
            "connection_close_seconds": 0.01,
            "server_stop_seconds": 0.02,
            "server_stop_outcome": "drained",
            "server_stop_initial_connections": 0,
            "server_stop_forced_connections": 0,
            "churn_attempted_connections": 0,
            "churn_accepted_connections": 0,
            "churn_connect_failures": 0,
            "churn_closed_connections": 0,
            "churn_batches": 0,
            "churn_elapsed_seconds": None,
            "churn_attempts_per_second": None,
            "churn_accepts_per_second": None,
            "churn_closes_per_second": None,
            "churn_worker_accept_counts": [],
            "churn_worker_skew_ratio": None,
            "churn_connect_p99_us": None,
            "churn_connect_max_us": None,
            "churn_accept_p99_us": None,
            "churn_accept_max_us": None,
            "churn_close_p99_us": None,
            "churn_close_max_us": None,
            "churn_schedule_lag_p99_us": None,
            "churn_schedule_lag_max_us": None,
            "churn_close_reason_counts": {
                "peer_eof": 0,
                "reset": 0,
                "connect_timeout": 0,
                "input_limit": 0,
                "output_overload": 0,
                "admission_policy": 0,
                "graceful_shutdown": 0,
                "forced_shutdown": 0,
                "callback_failure": 0,
                "internal_error": 0,
            },
            "requested_bytes": 67108864,
            "accepted_bytes": 0,
            "rejected_bytes": 67108864,
            "accepted_sends": 0,
            "rejected_sends": 2,
            "overloaded_sends": 2,
            "closed_sends": 0,
            "owner_unavailable_sends": 0,
            "output_hard_limit_bytes": 16777216,
            "pending_output_peak_bytes": 0,
            "read_pause_observations": 0,
            "read_resume_observations": 0,
            "backpressure_recovery_seconds": None,
            "high_water_callbacks": 0,
        },
    }
    runner.validate_document(
        semantic_document,
        slow_scenario,
        "windows",
        "iocp",
        "Release",
    )
    invalid_semantics = json.loads(json.dumps(semantic_document))
    invalid_semantics["measurements"]["rejected_bytes"] -= 1
    try:
        runner.validate_document(
            invalid_semantics,
            slow_scenario,
            "windows",
            "iocp",
            "Release",
        )
    except runner.MatrixError:
        pass
    else:
        raise AssertionError("v2 benchmark accounting mismatch was not rejected")

    with tempfile.TemporaryDirectory(prefix="gamenet-performance-regression-") as directory:
        root = Path(directory)
        baseline = root / "baseline"
        candidate = root / "candidate"
        output = root / "regression.json"
        write_matrix(
            baseline,
            BASELINE_SHA,
            runner,
            budgets,
            legacy_core_v1=True,
        )
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
        core_comparison = next(
            item for item in evidence["comparisons"]
            if item["key"].startswith("core.")
        )
        assert core_comparison["baseline_schema"] == "gamenet.core_benchmark.v1"
        assert core_comparison["candidate_schema"] == "gamenet.core_benchmark.v2"
        assert core_comparison["parameters"]["v2_only_default"] == 1

        candidate_drift = root / "candidate-drift"
        write_matrix(
            candidate_drift,
            CANDIDATE_SHA,
            runner,
            budgets,
            legacy_parameter_drift=True,
        )
        drift_command = list(command)
        drift_command[drift_command.index(str(candidate))] = str(candidate_drift)
        drift = subprocess.run(
            drift_command,
            capture_output=True,
            text=True,
            check=False,
        )
        assert drift.returncode == 2, drift.stderr
        assert "legacy parameter fixture_key differs" in drift.stderr

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
    assert workflow_text.count("tools/run_paired_performance_matrix.py") >= 4
    assert workflow_text.count("tools/compare_performance_regression.py") >= 2
    assert workflow_text.count("performance-regression.json") >= 2

    print("performance regression matrix and failure fixtures verified")


if __name__ == "__main__":
    main()
