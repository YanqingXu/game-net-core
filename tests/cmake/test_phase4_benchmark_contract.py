from __future__ import annotations

import json
import sys
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing Phase 4 benchmark contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    top_cmake = repo_root / "CMakeLists.txt"
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    benchmark_source = repo_root / "benchmarks" / "phase4" / "main.cpp"
    validator = repo_root / "tools" / "validate_phase4_benchmark.py"
    intent_index = repo_root / "intents" / "README.md"
    intent = repo_root / "intents" / "usecases" / "phase4_performance_baseline.intent.md"
    docs = repo_root / "docs" / "development" / "phase4_benchmark.md"
    local_results = (
        repo_root
        / "docs"
        / "development"
        / "benchmark_results"
        / "2026-07-11-windows-msvc-release-phase4"
    )
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    long_soak = repo_root / ".github" / "workflows" / "long-soak.yml"
    workflow_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    long_soak_contract = repo_root / "tests" / "ci" / "test_long_soak_workflow.py"
    ci_docs = repo_root / "docs" / "development" / "ci.md"

    top_text = top_cmake.read_text(encoding="utf-8")
    cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    source_text = benchmark_source.read_text(encoding="utf-8")
    validator_text = validator.read_text(encoding="utf-8")
    intent_text = intent.read_text(encoding="utf-8")
    docs_text = docs.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    long_soak_text = long_soak.read_text(encoding="utf-8")
    workflow_contract_text = workflow_contract.read_text(encoding="utf-8")
    long_soak_contract_text = long_soak_contract.read_text(encoding="utf-8")
    ci_docs_text = ci_docs.read_text(encoding="utf-8")

    require(
        top_text,
        'option(GAMENET_BUILD_BENCHMARKS "Build opt-in performance benchmarks" OFF)',
        top_cmake,
    )
    require(top_text, "if(GAMENET_BUILD_BENCHMARKS)", top_cmake)
    require(cmake_text, "add_executable(gamenet_phase4_benchmark", benchmark_cmake)
    for target in (
        "GameNet::protocol",
        "GameNet::game_session",
        "GameNet::game_logic",
        "GameNet::broadcast",
    ):
        require(cmake_text, target, benchmark_cmake)
    require(cmake_text, "GAMENET_BENCHMARK_BUILD_TYPE", benchmark_cmake)
    assert "add_test(" not in cmake_text, "benchmarks must not be registered as CTest"
    assert "install(" not in cmake_text, "benchmarks must not enter the install surface"

    require(
        intent_index.read_text(encoding="utf-8"),
        "intents/usecases/phase4_performance_baseline.intent.md",
        intent_index,
    )
    for fragment in (
        "artifact_kind: benchmark",
        "gamenet.phase4_benchmark.v1",
        "Framing runs entirely on the main thread",
        "Producer threads are",
        "Cross-loop completion",
        "process-level sampled RSS",
        "is not a CTest",
    ):
        require(intent_text, fragment, intent)

    for fragment in (
        "gamenet.phase4_benchmark.v1",
        '"framing"',
        '"logic-queue"',
        '"broadcast-fanout"',
        '"session-expiry-scan"',
        '"session-expiry"',
        '"timer-storm"',
        "PacketFramer",
        "SessionManager",
        "LogicLoop",
        "BroadcastRouter",
        "BroadcastDispatcher",
        "EventLoopThread",
        "isInOwnerThread",
        "throughput_mib_per_second",
        "logic_queue_lag_p99_us",
        "logic_depth_high_watermark",
        "logic_bytes_high_watermark",
        "broadcast_endpoint_latency_p99_us",
        "broadcast_route_latency_p99_us",
        "broadcast_queue_latency_p99_us",
        "broadcast_fanout_completion_p99_us",
        "broadcast_accepted",
        "broadcast_dropped",
        "broadcast_task_high_watermark",
        "working_set_peak_bytes",
        "working_set_peak_delta_bytes",
        "session_expiry_ns_per_session",
        "session_expired",
        "session_remaining",
        "session_close_requests",
        "runTimerStorm",
        "EventLoopMetricEvent::TimersDrained",
        "maxTimersPerIteration",
        "timer_callback_lag_p99_us",
        "timer_oldest_ready_latency_p99_us",
        "timer_metric_drained",
        "timer_budget_exhausted_iterations",
        "timer_remaining_high_watermark",
        "WorkingSetSampler",
        "std::vector<std::jthread> producers",
        "FanoutProbe must outlive owner-loop tasks",
        "ownerThreads stop/join before probe destruction",
        "GetProcessMemoryInfo",
        'std::ifstream statm("/proc/self/statm")',
    ):
        require(source_text, fragment, benchmark_source)

    for fragment in (
        "gamenet.phase4_benchmark.v1",
        "gamenet.phase4_benchmark_evidence.v1",
        "validate_framing",
        "validate_logic_queue",
        "validate_session_expiry",
        "validate_broadcast_fanout",
        "validate_timer_storm",
        "logic accepted count must equal configured messages",
        "broadcast accepted count must equal messages times fanout",
        "timer drain iterations must equal ceil(messages / batch size)",
        "timer metric-drained count must equal configured messages",
        "timer budget-exhausted iterations must cover every non-final drain",
        "working set peak delta must equal peak minus before",
        "sha256",
        "no performance thresholds",
    ):
        require(validator_text, fragment, validator)

    for fragment in (
        "-DGAMENET_BUILD_BENCHMARKS=ON",
        "--scenario framing",
        "--scenario logic-queue",
        "--scenario broadcast-fanout",
        "--scenario session-expiry-scan",
        "--scenario session-expiry",
        "--scenario timer-storm",
        "no-ready-bucket",
        "timer_callback_lag_p99_us",
        "EventLoopExecutor::tryQueue()",
        "P99",
        "working set",
        "Raw JSON",
        "does not define performance thresholds",
        "tools/validate_phase4_benchmark.py",
        "count invariants",
        "workflow run id",
    ):
        require(docs_text, fragment, docs)
    require(
        docs_text,
        "benchmark_results/2026-07-11-windows-msvc-release-phase4/",
        docs,
    )

    result_files = sorted(local_results.glob("*.json"))
    assert len(result_files) == 3, f"expected three local Phase 4 benchmark documents: {result_files}"
    observed_scenarios: set[str] = set()
    for result_file in result_files:
        document = json.loads(result_file.read_text(encoding="utf-8"))
        assert document["schema"] == "gamenet.phase4_benchmark.v1", result_file
        assert document["status"] == "ok", result_file
        assert document["platform"] == "windows", result_file
        assert document["backend"] == "iocp", result_file
        assert document["build_type"] == "Release", result_file
        observed_scenarios.add(document["scenario"])
        measurements = document["measurements"]
        for key in (
            "throughput_mib_per_second",
            "logic_queue_lag_p99_us",
            "logic_depth_high_watermark",
            "broadcast_queue_latency_p99_us",
            "broadcast_fanout_completion_p99_us",
            "broadcast_accepted",
            "broadcast_dropped",
            "broadcast_task_high_watermark",
            "working_set_peak_bytes",
        ):
            assert key in measurements, f"missing {key} in {result_file}"
    assert observed_scenarios == {"framing", "logic-queue", "broadcast-fanout"}

    sys.path.insert(0, str(repo_root / "tools"))
    import validate_phase4_benchmark as phase4_validator

    session_parameters = {
        "messages": 1000,
        "payload_bytes": 1,
        "threads": 1,
        "batch_size": 1,
        "fanout": 1,
        "tick_interval_us": 1000,
        "timeout_ms": 30000,
    }
    for scenario, visited, expired, remaining, closes in (
        ("session-expiry-scan", 0, 0, 1000, 0),
        ("session-expiry", 1000, 1000, 0, 1000),
    ):
        phase4_validator.validate_document(
            {
                "schema": "gamenet.phase4_benchmark.v1",
                "status": "ok",
                "error": None,
                "scenario": scenario,
                "platform": "windows",
                "backend": "iocp",
                "build_type": "Release",
                "parameters": session_parameters,
                "measurements": {
                    "elapsed_ms": 1.0,
                    "operations_per_second": 1_000_000.0,
                    "session_expiry_ns_per_session": 1000.0,
                    "session_visited": visited,
                    "session_expired": expired,
                    "session_remaining": remaining,
                    "session_close_requests": closes,
                },
            },
            scenario,
            "windows",
            "iocp",
            "Release",
            Path(f"{scenario}.json"),
        )

    timer_document = {
        "schema": "gamenet.phase4_benchmark.v1",
        "status": "ok",
        "error": None,
        "scenario": "timer-storm",
        "platform": "windows",
        "backend": "iocp",
        "build_type": "Release",
        "parameters": {
            **session_parameters,
            "messages": 10000,
            "batch_size": 1024,
        },
        "measurements": {
            "elapsed_ms": 10.0,
            "operations_per_second": 1_000_000.0,
            "timer_callback_lag_p50_us": 100.0,
            "timer_callback_lag_p99_us": 200.0,
            "timer_callback_lag_max_us": 250.0,
            "timer_oldest_ready_latency_p50_us": 110.0,
            "timer_oldest_ready_latency_p99_us": 210.0,
            "timer_callbacks": 10000,
            "timer_metric_drained": 10000,
            "timer_drain_iterations": 10,
            "timer_budget_exhausted_iterations": 9,
            "timer_remaining_high_watermark": 8976,
            "working_set_before_bytes": 1000000,
            "working_set_after_bytes": 1200000,
            "working_set_peak_bytes": 1500000,
            "working_set_peak_delta_bytes": 500000,
        },
    }
    phase4_validator.validate_document(
        timer_document,
        "timer-storm",
        "windows",
        "iocp",
        "Release",
        Path("timer-storm.json"),
    )
    invalid_timer_documents = (
        ("timer_callbacks", 9999),
        ("timer_metric_drained", 9999),
        ("timer_drain_iterations", 9),
        ("timer_budget_exhausted_iterations", 10),
        ("timer_remaining_high_watermark", 8975),
        ("timer_callback_lag_p99_us", 99.0),
    )
    for key, value in invalid_timer_documents:
        mutated = json.loads(json.dumps(timer_document))
        mutated["measurements"][key] = value
        try:
            phase4_validator.validate_document(
                mutated,
                "timer-storm",
                "windows",
                "iocp",
                "Release",
                Path(f"invalid-{key}.json"),
            )
        except phase4_validator.BenchmarkValidationError:
            pass
        else:
            raise AssertionError(f"timer validator accepted invalid {key}")

    linux_guard = "python3 tests/cmake/test_phase4_benchmark_contract.py"
    windows_guard = "python tests/cmake/test_phase4_benchmark_contract.py"
    require(workflow_text, linux_guard, workflow)
    require(workflow_text, windows_guard, workflow)
    require(long_soak_text, linux_guard, long_soak)
    require(workflow_contract_text, linux_guard, workflow_contract)
    require(workflow_contract_text, windows_guard, workflow_contract)
    require(long_soak_contract_text, linux_guard, long_soak_contract)
    require(ci_docs_text, "Phase 4 benchmark contract guard", ci_docs)


if __name__ == "__main__":
    main()
