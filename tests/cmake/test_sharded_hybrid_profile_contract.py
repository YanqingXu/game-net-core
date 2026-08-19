from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing sharded Hybrid contract in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    source = repo_root / "examples/runtime_profiles/MultiIoShardedHybrid.cc"
    header = repo_root / "examples/runtime_profiles/MultiIoShardedHybrid.h"
    example = repo_root / "examples/runtime_profiles/multi_io_sharded_hybrid_echo.cpp"
    benchmark = repo_root / "benchmarks/runtime_profiles/multi_io_sharded_hybrid.cpp"
    contract = (
        repo_root
        / "tests/contract/runtime_model/test_sharded_hybrid_profile.cpp"
    )
    profile_intent = (
        repo_root
        / "intents/usecases/multi_io_sharded_hybrid_profile.intent.md"
    )
    examples_cmake = repo_root / "examples/CMakeLists.txt"
    benchmarks_cmake = repo_root / "benchmarks/CMakeLists.txt"
    tests_cmake = repo_root / "tests/CMakeLists.txt"

    for path in (source, header, example, benchmark, contract, profile_intent):
        assert path.is_file(), f"missing sharded Hybrid artifact: {path}"

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
        "server_.setLoopSelectionPolicy",
        "selectLogicShard",
        "stableHash",
        "CellQueue",
        "maxCommandsPerCell",
        "maxQueuedBytesPerCell",
        "HybridDispatchLane::EventDriven",
        "HybridDispatchLane::FixedTick",
        "RepeatingTimerMode::FixedRate",
        "drainEventPrefix",
        "drainTickPrefix",
        "fixedHeadDeferrals",
        "route->ownerExecutor.post",
        "route->isCurrent",
        "closeAndDiscard",
        "completeLogicStop",
    ):
        require(source_text, fragment, source)
    for forbidden in (
        "runInLoop",
        "registerControlSource",
        "RepeatingTimerMode::FixedDelay",
        "logicLoops[network",
        "logicLoops[worker",
    ):
        assert forbidden not in source_text, (
            f"sharded Hybrid contains forbidden coupled/unbounded path: {forbidden}"
        )

    for fragment in (
        "ConnectionPlacementPolicy",
        "LogicShardPolicy",
        "LogicShardKeyKind",
        "LogicShardKey",
        "HybridDispatchLane",
        "MultiIoShardedHybridRoute",
        "MultiIoShardedHybridContext",
        "maxCommandsPerCell",
        "maxQueuedBytesPerCell",
        "maxShardKeyBytes",
        "maxCommandsPerEventDrain",
        "maxCommandsPerTick",
        "MultiIoShardedHybridStopHandle",
        "std::vector<ShardedHybridCellMetrics>",
    ):
        require(header_text, fragment, header)

    require(example_text, "MultiIoShardedHybrid", example)
    require(examples_cmake_text, "MultiIoShardedHybrid.cc", examples_cmake)
    require(examples_cmake_text, "gamenet_multi_io_sharded_hybrid_echo", examples_cmake)
    assert "install(TARGETS gamenet_multi_io_sharded_hybrid" not in examples_cmake_text
    assert not (repo_root / "include/gamenet/runtime_profile").exists()

    for fragment in (
        "event_messages_per_second",
        "fixed_tick_messages_per_second",
        "cell_imbalance_ratio",
        "queue_age_p999_us",
        "cross_domain_handoffs",
        "working_set_delta_bytes",
        "shutdown_us",
    ):
        require(benchmark_text, fragment, benchmark)
    require(
        benchmarks_cmake_text,
        "gamenet_multi_io_sharded_hybrid_benchmark",
        benchmarks_cmake,
    )
    require(tests_cmake_text, "test_sharded_hybrid_profile.cpp", tests_cmake)

    for fragment in (
        "networkOwnerCount == 2",
        "logicCellCount == 2",
        "connectionOwnerMigrations == 0",
        "orderViolations == 0",
        "fixedHeadDeferrals >= 1",
        "queueFullRejections >= 1",
        "staleInputs >= 2",
        "logicStop.wait_for",
        "cellsRetired == 2",
    ):
        require(contract_text, fragment, contract)

    for fragment in (
        "ConnectionPlacementPolicy",
        "LogicShardPolicy",
        "player, room, or scene",
        "strictly in cell-sequence order",
        "never overtakes",
        "not installed",
    ):
        require(intent_text, fragment, profile_intent)


if __name__ == "__main__":
    main()
