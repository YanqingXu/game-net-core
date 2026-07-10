from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing core benchmark contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    top_cmake = repo_root / "CMakeLists.txt"
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    benchmark_source = repo_root / "benchmarks" / "core" / "main.cpp"
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
        'option(GAMENET_BUILD_BENCHMARKS "Build core performance benchmarks" OFF)',
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
    require(intent_text, "gamenet.core_benchmark.v1", intent)
    require(intent_text, "notification-only behavior", intent)
    require(scope_text, '"benchmarks"', scope_guard)

    for fragment in (
        "gamenet.core_benchmark.v1",
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
        "high_water_callbacks",
        "high_water_notification_only",
        "single_get_queued_completion_status",
        "epoll_wait_batch",
    ):
        require(source_text, fragment, benchmark_source)

    require(source_text, "GetProcessMemoryInfo", benchmark_source)
    require(source_text, 'std::ifstream statm("/proc/self/statm")', benchmark_source)
    require(source_text, "server.connectionCount() == 0", benchmark_source)

    require(docs_text, "-DGAMENET_BUILD_BENCHMARKS=ON", docs)
    require(docs_text, "--scenario echo", docs)
    require(docs_text, "--scenario connections", docs)
    require(docs_text, "--scenario slow-client", docs)
    require(docs_text, "Raw JSON evidence", docs)

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
