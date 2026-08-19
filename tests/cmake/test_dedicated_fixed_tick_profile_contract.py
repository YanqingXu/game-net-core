from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing fixed-tick Profile contract in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    source = repo_root / "examples" / "runtime_profiles" / "MultiIoDedicatedFixedTick.cc"
    header = repo_root / "examples" / "runtime_profiles" / "MultiIoDedicatedFixedTick.h"
    example = repo_root / "examples" / "runtime_profiles" / "multi_io_fixed_tick_echo.cpp"
    benchmark = repo_root / "benchmarks" / "runtime_profiles" / "multi_io_fixed_tick.cpp"
    contract = (
        repo_root
        / "tests"
        / "contract"
        / "runtime_model"
        / "test_dedicated_fixed_tick_profile.cpp"
    )
    profile_intent = (
        repo_root
        / "intents"
        / "usecases"
        / "multi_io_dedicated_fixed_tick_profile.intent.md"
    )
    examples_cmake = repo_root / "examples" / "CMakeLists.txt"
    benchmarks_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"

    for path in (source, header, example, benchmark, contract, profile_intent):
        assert path.is_file(), f"missing fixed-tick Profile artifact: {path}"

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
        "tryRunEvery",
        "RepeatingTimerMode::FixedRate",
        "FixedTickCadence::FixedRateSkipMissed",
        "FixedTickCadence::FixedRateBoundedCatchUp",
        "maxCatchUpTicks",
        "maxCommandsPerTick",
        "route->isCurrent",
        "route->ownerExecutor.post",
        "SubmitResult::QueueFull",
        "PostResult::QueueFull",
        "EndpointResult::Overloaded",
        "stopCadenceOnOwner",
        "cadenceStopResultPublished",
        "cadenceStopResultPublished.store(true, std::memory_order_release)",
        "!state->cadenceStopResultPublished.load(std::memory_order_acquire)",
        "completeLogicStop",
        "closeAndDiscard",
    ):
        require(source_text, fragment, source)
    for forbidden in (
        "runInLoop",
        "registerControlSource",
        "scheduleProducerDrain",
        "RepeatingTimerMode::FixedDelay",
    ):
        assert forbidden not in source_text, (
            f"fixed-tick Profile contains forbidden execution path: {forbidden}"
        )

    for fragment in (
        "MultiIoDedicatedFixedTickOptions",
        "ioThreads{2}",
        "tickInterval",
        "maxCommandsPerTick",
        "FixedRateSkipMissed",
        "FixedRateBoundedCatchUp",
        "maxCatchUpTicks",
        "tickJitterP50Us",
        "tickJitterP99Us",
        "tickJitterP999Us",
        "tickDurationP99Us",
        "queueAgeP999Us",
        "shutdownDrainWaitUs",
        "MultiIoDedicatedFixedTickStopHandle",
    ):
        require(header_text, fragment, header)

    require(example_text, "MultiIoDedicatedFixedTick", example)
    require(examples_cmake_text, "MultiIoDedicatedFixedTick.cc", examples_cmake)
    require(examples_cmake_text, "gamenet_multi_io_fixed_tick_echo", examples_cmake)
    assert "install(TARGETS gamenet_multi_io_fixed_tick" not in examples_cmake_text
    assert not (repo_root / "include" / "gamenet" / "runtime_profile").exists()

    require(benchmark_text, "tick_jitter_p999_us", benchmark)
    require(benchmark_text, "tick_duration_p999_us", benchmark)
    require(benchmark_text, "skipped_ticks", benchmark)
    require(benchmark_text, "catch_up_ticks", benchmark)
    require(benchmark_text, "working_set_delta_bytes", benchmark)
    require(
        benchmarks_cmake_text,
        "gamenet_multi_io_fixed_tick_benchmark",
        benchmarks_cmake,
    )
    require(tests_cmake_text, "test_dedicated_fixed_tick_profile.cpp", tests_cmake)

    for fragment in (
        "networkOwnerCount == 2",
        "maxCommandsInTick == 2",
        "catchUpTicks >= 1",
        "skippedTicks >= 1",
        "maxConsecutiveCatchUp == 1",
        "queueFullRejections >= 1",
        "staleOutputs >= 1",
        "logicStop.wait_for",
        "future_status::ready",
    ):
        require(contract_text, fragment, contract)

    for fragment in (
        "FixedRateSkipMissed",
        "FixedRateBoundedCatchUp",
        "TimerQueue",
        "maxCommandsPerTick",
        "logic future",
        "not installed",
    ):
        require(intent_text, fragment, profile_intent)


if __name__ == "__main__":
    main()
