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
