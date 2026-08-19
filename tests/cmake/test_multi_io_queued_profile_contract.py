from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing MultiIoQueuedEvent contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    source = repo_root / "examples" / "runtime_profiles" / "MultiIoQueuedEvent.cc"
    header = repo_root / "examples" / "runtime_profiles" / "MultiIoQueuedEvent.h"
    example = repo_root / "examples" / "runtime_profiles" / "multi_io_queued_echo.cpp"
    benchmark = repo_root / "benchmarks" / "runtime_profiles" / "multi_io_queued.cpp"
    contract = (
        repo_root
        / "tests"
        / "contract"
        / "runtime_model"
        / "test_network_logic_split_profile.cpp"
    )
    profile_intent = (
        repo_root
        / "intents"
        / "usecases"
        / "multi_io_queued_event_profile.intent.md"
    )
    examples_cmake = repo_root / "examples" / "CMakeLists.txt"
    benchmarks_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"

    for path in (source, header, example, benchmark, contract, profile_intent):
        assert path.is_file(), f"missing Profile B artifact: {path}"

    source_text = source.read_text(encoding="utf-8")
    header_text = header.read_text(encoding="utf-8")
    example_text = example.read_text(encoding="utf-8")
    benchmark_text = benchmark.read_text(encoding="utf-8")
    contract_text = contract.read_text(encoding="utf-8")
    intent_text = profile_intent.read_text(encoding="utf-8")
    examples_cmake_text = examples_cmake.read_text(encoding="utf-8")
    benchmarks_cmake_text = benchmarks_cmake.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")

    for fragment in (
        "GameCommandQueue",
        "server_.setThreadNum(callbackState_->options.ioThreads)",
        "maxCommandsPerDrain",
        "drainScheduled.exchange",
        "logicExecutor.post",
        "route->isCurrent",
        "route->ownerExecutor.post",
        "SubmitResult::QueueFull",
        "PostResult::QueueFull",
        "EndpointResult::Overloaded",
        "completeLogicStop",
        "closeAndDiscard",
    ):
        require(source_text, fragment, source)
    assert "runInLoop" not in source_text, (
        "Profile B must not use inline/recursive cross-domain fallback"
    )
    assert "registerControlSource" not in source_text, (
        "Profile B business work must not bypass normal queue admission"
    )

    for fragment in (
        "MultiIoQueuedEventOptions",
        "ioThreads{2}",
        "maxCommandsPerDrain",
        "networkToLogicP99Us",
        "networkToLogicP999Us",
        "logicToNetworkP99Us",
        "logicToNetworkP999Us",
        "queueOldestAgeMaxUs",
        "MultiIoQueuedStopHandle",
    ):
        require(header_text, fragment, header)

    require(example_text, "MultiIoQueuedEvent", example)
    require(examples_cmake_text, "MultiIoQueuedEvent.cc", examples_cmake)
    require(examples_cmake_text, "gamenet_multi_io_queued_echo", examples_cmake)
    require(benchmark_text, "network_to_logic_p999_us", benchmark)
    require(benchmark_text, "logic_to_network_p999_us", benchmark)
    require(benchmark_text, "queue_oldest_age_max_us", benchmark)
    require(benchmark_text, "working_set_delta_bytes", benchmark)
    require(benchmarks_cmake_text, "gamenet_multi_io_queued_benchmark", benchmarks_cmake)
    assert "install(TARGETS gamenet_multi_io_queued" not in examples_cmake_text
    assert not (repo_root / "include" / "gamenet" / "runtime_profile").exists(), (
        "Profile B must remain non-installed while common Profile API is unreviewed"
    )

    require(tests_cmake_text, "test_network_logic_split_profile.cpp", tests_cmake)
    for fragment in (
        "networkOwnerCount == 2",
        "producerWakePosts == 1",
        "producerWakeMerges == 9",
        "maxCommandsInDrain == 2",
        "queueFullRejections >= 1",
        "staleOutputs >= 1",
        "crossDomainHandoffs == 20",
        "logicStop.wait_for",
        "future_status::ready",
    ):
        require(contract_text, fragment, contract)

    for fragment in (
        "ioThreads >= 2",
        "GameCommandQueue",
        "first accepted command",
        "P99/P999",
        "logic future",
    ):
        require(intent_text, fragment, profile_intent)


if __name__ == "__main__":
    main()
