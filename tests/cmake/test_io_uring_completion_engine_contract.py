import json
import sys
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing IOE-X1 fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    root_cmake = repo_root / "CMakeLists.txt"
    experimental_cmake = repo_root / "src/experimental/io_uring/CMakeLists.txt"
    engine_header = repo_root / "src/experimental/io_uring/IoUringCompletionEngine.h"
    engine_source = repo_root / "src/experimental/io_uring/IoUringCompletionEngine.cc"
    core_cmake = repo_root / "src/core/CMakeLists.txt"
    adapter = repo_root / "src/core/net/detail/PollerIoEngineAdapter.cc"
    tests_cmake = repo_root / "tests/CMakeLists.txt"
    contract = repo_root / "tests/contract/io_engine/test_io_uring_completion_engine.cpp"
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    benchmark = repo_root / "benchmarks" / "io_uring" / "one_shot.cpp"
    benchmark_validator = repo_root / "tools" / "validate_io_uring_benchmark.py"
    benchmark_docs = repo_root / "docs" / "development" / "io_uring_benchmark.md"
    intent = repo_root / "intents/architecture/io_engine.intent.md"
    thread_rules = repo_root / "rules/thread_affinity_rules.md"
    ownership_rules = repo_root / "rules/ownership_rules.md"
    testing_rules = repo_root / "rules/testing_rules.md"
    workflow = repo_root / ".github/workflows/ci.yml"
    platform_docs = repo_root / "docs/development/platform_support.md"

    for path in (
        experimental_cmake,
        engine_header,
        engine_source,
        contract,
        benchmark,
        benchmark_validator,
        benchmark_docs,
    ):
        assert path.is_file(), f"missing IOE-X1 artifact: {path}"

    root_text = root_cmake.read_text(encoding="utf-8")
    require(root_text, "GAMENET_ENABLE_EXPERIMENTAL requires Linux", root_cmake)
    require(root_text, "add_subdirectory(src/experimental/io_uring)", root_cmake)
    assert root_text.index("if(GAMENET_ENABLE_EXPERIMENTAL)") < root_text.index(
        "add_subdirectory(src/experimental/io_uring)"
    )

    cmake_text = experimental_cmake.read_text(encoding="utf-8")
    require(cmake_text, "add_library(gamenet_experimental_io_uring STATIC", experimental_cmake)
    require(cmake_text, "add_library(GameNet::experimental ALIAS", experimental_cmake)
    require(cmake_text, "gamenet_configure_sanitizers", experimental_cmake)
    assert "install(" not in cmake_text, "IOE-X1 target must remain non-installed"

    combined = engine_header.read_text(encoding="utf-8") + engine_source.read_text(encoding="utf-8")
    for fragment in (
        "__NR_io_uring_setup",
        "__NR_io_uring_enter",
        "__NR_io_uring_register",
        "IORING_OP_ACCEPT",
        "IORING_OP_RECV",
        "IORING_OP_SEND",
        "IORING_OP_ASYNC_CANCEL",
        "SubmissionQueueFull",
        "maxOperations",
        "maxOwnedBytes",
        "generation",
        "beginQuiesce",
        "IoUringShutdownResult",
    ):
        require(combined, fragment, engine_source)

    for forbidden in (
        "IORING_ACCEPT_MULTISHOT",
        "IORING_RECV_MULTISHOT",
        "IOSQE_BUFFER_SELECT",
        "IORING_REGISTER_BUFFERS",
        "IORING_REGISTER_FILES",
        "IORING_OP_PROVIDE_BUFFERS",
        "IORING_OP_SEND_ZC",
        "IORING_SETUP_SQPOLL",
        "IOSQE_IO_LINK",
        "IOSQE_IO_HARDLINK",
    ):
        assert forbidden not in combined, f"IOE-X1 advanced feature escaped scope: {forbidden}"

    core_text = core_cmake.read_text(encoding="utf-8")
    adapter_text = adapter.read_text(encoding="utf-8")
    assert "IoUring" not in core_text
    assert "IoUring" not in adapter_text
    require(adapter_text, '#include "EpollReadinessPort.h"', adapter)

    tests_text = tests_cmake.read_text(encoding="utf-8")
    require(tests_text, "if(GAMENET_ENABLE_EXPERIMENTAL)", tests_cmake)
    require(tests_text, "gamenet_io_uring_contract", tests_cmake)
    require(tests_text, "contract.io_engine.test_io_uring_completion_engine", tests_cmake)
    require(tests_text, "GameNet::experimental", tests_cmake)
    require(tests_text, "experimental;threading;lifecycle", tests_cmake)

    contract_text = contract.read_text(encoding="utf-8")
    for fragment in (
        "testFiniteSqRejectsWithoutFallback",
        "testOneShotAcceptRecvSend",
        "testCancelLeaseAndFinalDrain",
        "testForeignThreadMutationRejected",
        "SubmissionQueueFull",
        "IoUringCompletionStatus::Cancelled",
        "crossDomain",
    ):
        require(contract_text, fragment, contract)

    benchmark_cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    require(benchmark_cmake_text, "if(GAMENET_ENABLE_EXPERIMENTAL)", benchmark_cmake)
    require(
        benchmark_cmake_text,
        "add_executable(gamenet_io_uring_one_shot_benchmark",
        benchmark_cmake,
    )
    require(benchmark_cmake_text, "GameNet::experimental", benchmark_cmake)
    assert "add_test(" not in benchmark_cmake_text
    assert "install(" not in benchmark_cmake_text

    benchmark_text = benchmark.read_text(encoding="utf-8")
    for fragment in (
        "gamenet.io_uring_one_shot_benchmark.v1",
        "IoUringOperationKind::Send",
        "IoUringOperationKind::Receive",
        "operations_accepted",
        "terminal_notices",
        "cross_domain_fallbacks",
        "owned_bytes",
        "p999_latency_us",
        "IoUringShutdownResult::Drained",
    ):
        require(benchmark_text, fragment, benchmark)

    validator_text = benchmark_validator.read_text(encoding="utf-8")
    require(
        validator_text,
        'SCHEMA = "gamenet.io_uring_one_shot_benchmark.v1"',
        benchmark_validator,
    )
    require(
        validator_text,
        "operations accepted must equal twice round trips",
        benchmark_validator,
    )
    require(
        validator_text,
        "io_uring benchmark retained residual state",
        benchmark_validator,
    )
    require(validator_text, "io_uring benchmark used a fallback", benchmark_validator)

    sys.path.insert(0, str(repo_root / "tools"))
    import validate_io_uring_benchmark as benchmark_contract

    document = {
        "schema": "gamenet.io_uring_one_shot_benchmark.v1",
        "status": "ok",
        "build_type": "Release",
        "parameters": {"round_trips": 1000, "payload_bytes": 256, "depth": 32},
        "measurements": {
            "elapsed_seconds": 0.01,
            "messages_per_second": 100000.0,
            "operations_per_second": 200000.0,
            "throughput_mib_per_second": 48.828125,
            "p50_latency_us": 10,
            "p99_latency_us": 20,
            "p999_latency_us": 30,
            "working_set_before_bytes": 1000000,
            "working_set_after_bytes": 1004096,
            "working_set_delta_bytes": 4096,
            "shutdown_milliseconds": 0.1,
            "operations_accepted": 2000,
            "terminal_notices": 2000,
            "sq_full_rejections": 0,
            "cross_domain_fallbacks": 0,
            "active_operations": 0,
            "ready_notices": 0,
            "owned_bytes": 0,
        },
    }
    benchmark_contract.validate_document(document, require_release=True)
    invalid = json.loads(json.dumps(document))
    invalid["measurements"]["terminal_notices"] = 1999
    try:
        benchmark_contract.validate_document(invalid, require_release=True)
    except benchmark_contract.IoUringBenchmarkValidationError:
        pass
    else:
        raise AssertionError("io_uring validator accepted missing terminal notice")

    docs_text = benchmark_docs.read_text(encoding="utf-8")
    require(docs_text, "gamenet_io_uring_one_shot_benchmark", benchmark_docs)
    require(docs_text, "validate_io_uring_benchmark.py", benchmark_docs)
    require(docs_text, "directional", benchmark_docs)

    intent_text = intent.read_text(encoding="utf-8")
    require(intent_text, "IOE-X1 authorizes one experimental Linux completion vertical slice", intent)
    require(intent_text, "tests/contract/io_engine/test_io_uring_completion_engine.cpp", intent)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X1's raw io_uring Engine", thread_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X1 experimental target owns", ownership_rules)
    require(testing_rules.read_text(encoding="utf-8"), "real Linux io_uring fd", testing_rules)

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "test_io_uring_completion_engine_contract.py", workflow)
    require(workflow_text, "GAMENET_ENABLE_EXPERIMENTAL=ON", workflow)
    require(workflow_text, "contract.io_engine.test_io_uring_completion_engine", workflow)
    require(platform_docs.read_text(encoding="utf-8"), "IOE-X1 io_uring", platform_docs)

    print("IOE-X1 real one-shot io_uring build, scope, and lifecycle contracts verified")


if __name__ == "__main__":
    main()
