from __future__ import annotations

import json
import sys
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing core benchmark contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    top_cmake = repo_root / "CMakeLists.txt"
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    benchmark_source = repo_root / "benchmarks" / "core" / "main.cpp"
    benchmark_validator = repo_root / "tools" / "validate_core_benchmark.py"
    intent_index = repo_root / "intents" / "README.md"
    intent = repo_root / "intents" / "usecases" / "core_performance_baseline.intent.md"
    scope_guard = repo_root / "tools" / "check_scope_boundaries.py"
    docs = repo_root / "docs" / "development" / "core_benchmark.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    long_soak = repo_root / ".github" / "workflows" / "long-soak.yml"
    workflow_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    long_soak_contract = repo_root / "tests" / "ci" / "test_long_soak_workflow.py"
    ci_docs = repo_root / "docs" / "development" / "ci.md"

    top_text = top_cmake.read_text(encoding="utf-8")
    benchmark_cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    source_text = benchmark_source.read_text(encoding="utf-8")
    validator_text = benchmark_validator.read_text(encoding="utf-8")
    index_text = intent_index.read_text(encoding="utf-8")
    intent_text = intent.read_text(encoding="utf-8")
    scope_text = scope_guard.read_text(encoding="utf-8")
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
    require(top_text, "add_subdirectory(benchmarks)", top_cmake)

    require(benchmark_cmake_text, "add_executable(gamenet_core_benchmark", benchmark_cmake)
    require(
        benchmark_cmake_text,
        "target_link_libraries(gamenet_core_benchmark PRIVATE GameNet::core)",
        benchmark_cmake,
    )
    require(benchmark_cmake_text, "GAMENET_BENCHMARK_BUILD_TYPE", benchmark_cmake)
    assert "add_test(" not in benchmark_cmake_text, "benchmark must not be registered as CTest"
    assert "install(" not in benchmark_cmake_text, "benchmark must not enter the install surface"

    require(index_text, "intents/usecases/core_performance_baseline.intent.md", intent_index)
    require(intent_text, "is not a CTest", intent)
    require(intent_text, "process main thread owns and destroys EventLoop and TcpServer", intent)
    require(intent_text, "gamenet.core_benchmark.v2", intent)
    require(intent_text, "requested, accepted, and rejected bytes", intent)
    require(intent_text, "hold reads until the memory sample", intent)
    require(scope_text, '"benchmarks"', scope_guard)

    for fragment in (
        "gamenet.core_benchmark.v2",
        "echo",
        "connections",
        "slow-client",
        "throughput_mib_per_second",
        "p50_latency_us",
        "p99_latency_us",
        "working_set_before_bytes",
        "working_set_after_bytes",
        "working_set_delta_bytes",
        "approx_bytes_per_connection",
        "connection_establish_seconds",
        "connection_establish_per_second",
        "idle_observation_seconds",
        "idle_process_cpu_seconds",
        "idle_process_cpu_percent",
        "connection_close_seconds",
        "server_stop_seconds",
        "server_stop_outcome",
        "server_stop_initial_connections",
        "server_stop_forced_connections",
        "requested_bytes",
        "accepted_bytes",
        "rejected_bytes",
        "low_water_bytes",
        "hard_limit_bytes",
        "max_input_buffer_bytes",
        "output_hard_limit_bytes",
        "pending_output_peak_bytes",
        "read_pause_observations",
        "read_resume_observations",
        "backpressure_recovery_seconds",
        "high_water_callbacks",
        "bounded_output_hysteresis",
        "get_queued_completion_status_ex_batch_64",
        "epoll_wait_batch",
    ):
        require(source_text, fragment, benchmark_source)

    require(source_text, "connection->trySend(payload)", benchmark_source)
    require(source_text, "connection->trySend(buffer->peek(), readable)", benchmark_source)
    require(
        source_text,
        "config.connections = parseSize(value, option, 1, 100000)",
        benchmark_source,
    )
    require(
        source_text,
        "config.connectConcurrency = parseSize(value, option, 1, 1024)",
        benchmark_source,
    )
    require(
        source_text,
        "--connect-concurrency must not exceed --connections",
        benchmark_source,
    )
    require(
        source_text,
        "config.iocpAcceptDepth = parseSize(value, option, 1, 64)",
        benchmark_source,
    )
    require(source_text, "server.setIocpAcceptDepth", benchmark_source)
    require(
        source_text,
        "--preload-before-loop is supported only for connections",
        benchmark_source,
    )
    require(source_text, "state.waitForClientsCreated", benchmark_source)
    assert "connection->send(" not in source_text, "Core benchmark must not ignore send admission"
    require(source_text, "result.requestedBytes != result.acceptedBytes + result.rejectedBytes", benchmark_source)
    require(source_text, "GetProcessMemoryInfo", benchmark_source)
    require(source_text, "GetProcessTimes", benchmark_source)
    require(source_text, "CLOCK_PROCESS_CPUTIME_ID", benchmark_source)
    require(source_text, 'std::ifstream statm("/proc/self/statm")', benchmark_source)
    require(source_text, "server.connectionCount() != 0", benchmark_source)
    require(source_text, "server.stopGracefully", benchmark_source)
    require(source_text, "completionCheckQueued", benchmark_source)
    assert "loop.runEvery(" not in source_text, (
        "idle CPU benchmark must not contain a recurring coordination poll"
    )
    require(source_text, "client.closeAbortively()", benchmark_source)
    require(validator_text, 'SCHEMA = "gamenet.core_benchmark.v2"', benchmark_validator)
    require(
        validator_text,
        "requested bytes do not equal accepted plus rejected",
        benchmark_validator,
    )
    require(
        validator_text,
        "pending output peak exceeds hard limit",
        benchmark_validator,
    )
    require(
        validator_text,
        "benchmark connection owner was unavailable",
        benchmark_validator,
    )
    require(
        validator_text,
        "connection establishment rate is inconsistent",
        benchmark_validator,
    )
    require(
        validator_text,
        "idle CPU percent is inconsistent",
        benchmark_validator,
    )
    require(
        validator_text,
        "successful stop must drain",
        benchmark_validator,
    )

    require(docs_text, "-DGAMENET_BUILD_BENCHMARKS=ON", docs)
    require(docs_text, "--scenario echo", docs)
    require(docs_text, "--scenario connections", docs)
    require(docs_text, "--connect-concurrency 64", docs)
    require(docs_text, "--iocp-accept-depth 4", docs)
    require(docs_text, "--preload-before-loop 1", docs)
    require(
        docs_text,
        "--scenario connections --connections 1000",
        docs,
    )
    require(
        docs_text,
        "--scenario connections --connections 10000",
        docs,
    )
    require(docs_text, "structured idle-memory profile", docs)
    require(docs_text, "idle_process_cpu_percent", docs)
    require(docs_text, "server_stop_seconds", docs)
    require(docs_text, "--scenario slow-client", docs)
    require(docs_text, "Raw JSON evidence", docs)

    sys.path.insert(0, str(repo_root / "tools"))
    import validate_core_benchmark as core_validator

    connection_document = {
        "schema": "gamenet.core_benchmark.v2",
        "status": "ok",
        "error": None,
        "scenario": "connections",
        "platform": "windows",
        "backend": "iocp",
        "completion_mode": "get_queued_completion_status_ex_batch_64",
        "backpressure_policy": "bounded_output_hysteresis",
        "build_type": "Release",
        "parameters": {
            "connections": 1000,
            "slow_bytes_per_connection": 8388608,
            "low_water_bytes": 32768,
            "high_water_bytes": 65536,
            "hard_limit_bytes": 16777216,
            "max_input_buffer_bytes": 2097152,
            "settle_ms": 1000,
        },
        "measurements": {
            "elapsed_seconds": 1.6,
            "round_trips": 0,
            "application_bytes": 0,
            "throughput_mib_per_second": None,
            "p50_latency_us": None,
            "p99_latency_us": None,
            "working_set_before_bytes": 1000000,
            "working_set_after_bytes": 2000000,
            "working_set_delta_bytes": 1000000,
            "approx_bytes_per_connection": 1000.0,
            "connection_establish_seconds": 0.5,
            "connection_establish_per_second": 2000.0,
            "idle_observation_seconds": 1.0,
            "idle_process_cpu_seconds": 0.02,
            "idle_process_cpu_percent": 2.0,
            "connection_close_seconds": 0.03,
            "server_stop_seconds": 0.04,
            "server_stop_outcome": "drained",
            "server_stop_initial_connections": 0,
            "server_stop_forced_connections": 0,
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
    core_validator.validate_document(connection_document)
    invalid_connection_documents = (
        ("connection_establish_per_second", 1900.0),
        ("idle_process_cpu_percent", 3.0),
        ("server_stop_outcome", "forced_after_timeout"),
        ("server_stop_initial_connections", 1),
        ("connection_close_seconds", None),
    )
    for key, value in invalid_connection_documents:
        mutated = json.loads(json.dumps(connection_document))
        mutated["measurements"][key] = value
        try:
            core_validator.validate_document(mutated)
        except core_validator.CoreBenchmarkValidationError:
            pass
        else:
            raise AssertionError(f"core validator accepted invalid {key}")

    linux_guard = "python3 tests/cmake/test_core_benchmark_contract.py"
    windows_guard = "python tests/cmake/test_core_benchmark_contract.py"
    require(workflow_text, linux_guard, workflow)
    require(workflow_text, windows_guard, workflow)
    require(long_soak_text, linux_guard, long_soak)
    require(workflow_contract_text, linux_guard, workflow_contract)
    require(long_soak_contract_text, linux_guard, long_soak_contract)
    require(ci_docs_text, "Core benchmark contract guard", ci_docs)


if __name__ == "__main__":
    main()
