from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path


CAPACITY_BASELINE_SHA = "bbcdd8af2e736d8f8ed53d49e787f14d7f7cb043"
CANDIDATE_SHA = "c" * 40
EXPECTED_KEYS = {
    "core.connections-1000",
    "core.connections-10000",
    *{
        f"core.echo-{payload}-{workers}-workers"
        for payload in (64, 256, 1024)
        for workers in (1, 2, 4)
    },
    "core.connection-churn-1000",
}


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_comparison_matrix(
    root: Path,
    sha: str,
    runner,
    budget: dict,
    scale: float = 1.0,
) -> None:
    root.mkdir()
    budget_by_key = {scenario["key"]: scenario for scenario in budget["scenarios"]}
    scenarios = []
    for scenario in runner.CAPACITY_SCENARIOS:
        key = f"{scenario.group}.{scenario.key}"
        parameters = {"fixture_key": key}
        samples = []
        for repetition in range(1, budget["repetitions"] + 1):
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
            relative = Path("core") / f"{scenario.key}-{repetition}.json"
            path = root / relative
            path.parent.mkdir(exist_ok=True)
            path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            samples.append({"path": relative.as_posix(), "sha256": sha256_file(path)})
        scenarios.append(
            {
                "key": key,
                "group": "core",
                "reported_scenario": scenario.reported_scenario,
                "schema": scenario.schema,
                "parameters": parameters,
                "samples": samples,
            }
        )
    manifest = {
        "schema": runner.SCHEMA,
        "profile": "core-capacity",
        "commit_sha": sha,
        "platform": "windows",
        "backend": "iocp",
        "build_type": "Release",
        "repetitions": budget["repetitions"],
        "executables": {
            "core": {"path": "fixture-core", "sha256": "1" * 64},
        },
        "scenarios": scenarios,
    }
    (root / "matrix-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )


def churn_document(rate: float, accept_p99_us: float) -> dict:
    attempted = 5000
    elapsed = attempted / rate
    connect_p99_us = 10000.0
    close_p99_us = 20000.0
    return {
        "schema": "gamenet.core_benchmark.v2",
        "status": "ok",
        "error": None,
        "scenario": "connection-churn",
        "platform": "linux",
        "backend": "epoll",
        "completion_mode": "epoll_wait_batch",
        "backpressure_policy": "bounded_output_hysteresis",
        "build_type": "Release",
        "parameters": {
            "connections": 100,
            "connect_concurrency": 16,
            "iocp_accept_depth": 32,
            "preload_before_loop": False,
            "event_loop_threads": 4,
            "messages_per_connection": 1000,
            "payload_bytes": 256,
            "churn_target_per_second": 1000,
            "churn_duration_ms": 5000,
            "churn_connect_timeout_ms": 1000,
            "slow_bytes_per_connection": 8388608,
            "low_water_bytes": 32768,
            "high_water_bytes": 65536,
            "hard_limit_bytes": 16777216,
            "max_input_buffer_bytes": 2097152,
            "settle_ms": 0,
            "timeout_ms": 60000,
        },
        "measurements": {
            "elapsed_seconds": elapsed,
            "round_trips": 0,
            "application_bytes": 0,
            "throughput_mib_per_second": None,
            "messages_per_second": None,
            "p50_latency_us": None,
            "p99_latency_us": None,
            "p999_latency_us": None,
            "working_set_before_bytes": 1000000,
            "working_set_after_bytes": 1100000,
            "working_set_delta_bytes": 100000,
            "approx_bytes_per_connection": 1000.0,
            "connection_establish_seconds": None,
            "connection_establish_per_second": None,
            "idle_observation_seconds": None,
            "idle_process_cpu_seconds": None,
            "idle_process_cpu_percent": None,
            "connection_close_seconds": 0.01,
            "server_stop_seconds": 0.01,
            "server_stop_outcome": "drained",
            "server_stop_initial_connections": 0,
            "server_stop_forced_connections": 0,
            "churn_attempted_connections": attempted,
            "churn_accepted_connections": attempted,
            "churn_connect_failures": 0,
            "churn_closed_connections": attempted,
            "churn_batches": 50,
            "churn_elapsed_seconds": elapsed,
            "churn_attempts_per_second": rate,
            "churn_accepts_per_second": rate,
            "churn_closes_per_second": rate,
            "churn_worker_accept_counts": [1250, 1250, 1250, 1250],
            "churn_worker_skew_ratio": 0.0,
            "churn_connect_p99_us": connect_p99_us,
            "churn_connect_max_us": connect_p99_us,
            "churn_accept_p99_us": accept_p99_us,
            "churn_accept_max_us": accept_p99_us,
            "churn_close_p99_us": close_p99_us,
            "churn_close_max_us": close_p99_us,
            "churn_schedule_lag_p99_us": max(
                connect_p99_us,
                accept_p99_us,
                close_p99_us,
            ),
            "churn_schedule_lag_max_us": max(
                connect_p99_us,
                accept_p99_us,
                close_p99_us,
            ),
            "churn_close_reason_counts": {
                "peer_eof": 0,
                "reset": attempted,
                "connect_timeout": 0,
                "input_limit": 0,
                "output_overload": 0,
                "admission_policy": 0,
                "graceful_shutdown": 0,
                "forced_shutdown": 0,
                "callback_failure": 0,
                "internal_error": 0,
            },
            "requested_bytes": 0,
            "accepted_bytes": 0,
            "rejected_bytes": 0,
            "accepted_sends": 0,
            "rejected_sends": 0,
            "overloaded_sends": 0,
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


def write_topology_matrix(root: Path, rate: float, accept_p99_us: float) -> None:
    sample_metadata = []
    parameters = None
    for repetition in range(1, 4):
        document = churn_document(rate, accept_p99_us)
        parameters = document["parameters"]
        relative = Path("core") / f"connection-churn-1000-{repetition}.json"
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        sample_metadata.append(
            {"path": relative.as_posix(), "sha256": sha256_file(path)}
        )
    manifest = {
        "schema": "gamenet.performance_matrix.v1",
        "profile": "core-capacity",
        "commit_sha": CANDIDATE_SHA,
        "platform": "linux",
        "backend": "epoll",
        "build_type": "Release",
        "repetitions": 3,
        "scenarios": [
            {
                "key": "core.connection-churn-1000",
                "group": "core",
                "reported_scenario": "connection-churn",
                "schema": "gamenet.core_benchmark.v2",
                "parameters": parameters,
                "samples": sample_metadata,
            }
        ],
    }
    (root / "matrix-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    runner = load_module(
        repo_root / "tools" / "run_performance_matrix.py",
        "gamenet_core_capacity_runner",
    )
    evaluator = load_module(
        repo_root / "tools" / "evaluate_core_accept_topology.py",
        "gamenet_core_accept_topology",
    )
    comparator = repo_root / "tools" / "compare_performance_regression.py"
    budget_path = repo_root / "benchmarks" / "core_capacity_regression_budgets.json"
    budget = json.loads(budget_path.read_text(encoding="utf-8"))

    assert len(runner.SCENARIOS) == 12, "frozen release regression inventory drifted"
    assert len(runner.CAPACITY_SCENARIOS) == 12
    assert budget["matrix_profile"] == "core-capacity"
    assert budget["baseline_sha"] == CAPACITY_BASELINE_SHA
    assert budget["repetitions"] == 3
    capacity_keys = {
        f"{scenario.group}.{scenario.key}"
        for scenario in runner.CAPACITY_SCENARIOS
    }
    assert capacity_keys == EXPECTED_KEYS
    assert {scenario["key"] for scenario in budget["scenarios"]} == EXPECTED_KEYS
    churn = next(
        scenario
        for scenario in runner.CAPACITY_SCENARIOS
        if scenario.key == "connection-churn-1000"
    )
    assert churn.require_zero_churn_failures
    assert "--churn-connect-timeout-ms" in churn.arguments
    legacy_capacity = churn_document(995.0, 1000.0)
    legacy_capacity["schema"] = "gamenet.core_benchmark.v1"
    try:
        runner.validate_document(
            legacy_capacity,
            churn,
            "linux",
            "epoll",
            "Release",
            allow_legacy_core_v1=False,
        )
    except runner.MatrixError:
        pass
    else:
        raise AssertionError("Core capacity profile accepted a legacy v1 artifact")

    with tempfile.TemporaryDirectory(prefix="gamenet-core-capacity-") as temp:
        root = Path(temp)
        baseline = root / "baseline"
        candidate = root / "candidate"
        output = root / "comparison.json"
        write_comparison_matrix(
            baseline,
            CAPACITY_BASELINE_SHA,
            runner,
            budget,
        )
        write_comparison_matrix(candidate, CANDIDATE_SHA, runner, budget)
        command = [
            sys.executable,
            str(comparator),
            "--baseline-root",
            str(baseline),
            "--candidate-root",
            str(candidate),
            "--budget",
            str(budget_path),
            "--baseline-sha",
            CAPACITY_BASELINE_SHA,
            "--candidate-sha",
            CANDIDATE_SHA,
            "--platform",
            "windows",
            "--backend",
            "iocp",
            "--output",
            str(output),
        ]
        passed = subprocess.run(command, capture_output=True, text=True, check=False)
        assert passed.returncode == 0, passed.stderr
        evidence = json.loads(output.read_text(encoding="utf-8"))
        assert evidence["matrix_profile"] == "core-capacity"
        assert len(evidence["comparisons"]) == 12

        failed_candidate = root / "failed-candidate"
        write_comparison_matrix(
            failed_candidate,
            CANDIDATE_SHA,
            runner,
            budget,
            scale=0.01,
        )
        failing_command = list(command)
        failing_command[failing_command.index(str(candidate))] = str(failed_candidate)
        failed = subprocess.run(
            failing_command,
            capture_output=True,
            text=True,
            check=False,
        )
        assert failed.returncode == 1

        retain_root = root / "retain"
        retain_root.mkdir()
        write_topology_matrix(retain_root, rate=995.0, accept_p99_us=1000.0)
        retain = evaluator.evaluate(retain_root, CANDIDATE_SHA)
        assert retain["result"] == "retain_single_listener"
        assert not retain["criteria"]["trigger_experiment"]

        experiment_root = root / "experiment"
        experiment_root.mkdir()
        write_topology_matrix(
            experiment_root,
            rate=900.0,
            accept_p99_us=60000.0,
        )
        experiment = evaluator.evaluate(experiment_root, CANDIDATE_SHA)
        assert experiment["result"] == "experiment_so_reuseport"
        assert experiment["criteria"]["rate_missed"]
        assert experiment["criteria"]["accept_dominant"]
        assert experiment["criteria"]["accept_saturated"]

    print("Core capacity matrix and accept-topology decision contracts verified")


if __name__ == "__main__":
    main()
