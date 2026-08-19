from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing Runtime Profile contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    source = repo_root / "examples" / "runtime_profiles" / "SingleLoopInlineEvent.cc"
    header = repo_root / "examples" / "runtime_profiles" / "SingleLoopInlineEvent.h"
    example = repo_root / "examples" / "runtime_profiles" / "single_loop_inline_echo.cpp"
    examples_cmake = repo_root / "examples" / "CMakeLists.txt"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    contract = (
        repo_root
        / "tests"
        / "contract"
        / "runtime_model"
        / "test_network_only_profile.cpp"
    )
    runtime_intent = repo_root / "intents" / "architecture" / "runtime_models.intent.md"
    profile_intent = (
        repo_root
        / "intents"
        / "usecases"
        / "single_loop_inline_event_profile.intent.md"
    )

    for path in (source, header, example, contract, runtime_intent, profile_intent):
        assert path.is_file(), f"missing Runtime Profile artifact: {path}"

    source_text = source.read_text(encoding="utf-8")
    header_text = header.read_text(encoding="utf-8")
    example_text = example.read_text(encoding="utf-8")
    examples_cmake_text = examples_cmake.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    contract_text = contract.read_text(encoding="utf-8")
    runtime_intent_text = runtime_intent.read_text(encoding="utf-8")
    profile_intent_text = profile_intent.read_text(encoding="utf-8")

    for fragment in (
        "server_.setThreadNum(0)",
        "maxFramesPerPush",
        "maxFrameBytesPerPush",
        "ownerExecutor.post",
        "PostResult::QueueFull",
        "EndpointResult::Overloaded",
        "maxHandlerWallTime",
        "callbackState->active",
        "stopGracefully",
    ):
        require(source_text, fragment, source)
    assert "runInLoop" not in source_text, (
        "Profile A continuation must queue once and never recurse inline"
    )

    for fragment in (
        "SingleLoopInlineEventOptions",
        "maxHandlerWallTime",
        "crossDomainHandoffs",
    ):
        require(header_text, fragment, header)

    require(example_text, "SingleLoopInlineEvent", example)
    require(example_text, "SingleLoopInlineHandlerResult", example)
    require(examples_cmake_text, "gamenet_runtime_profile_demo_support", examples_cmake)
    require(examples_cmake_text, "gamenet_single_loop_inline_echo", examples_cmake)
    assert "install(TARGETS gamenet_runtime_profile" not in examples_cmake_text
    assert not (repo_root / "include" / "gamenet" / "runtime_profile").exists(), (
        "Profile A must remain non-installed until two Profiles prove common API needs"
    )

    require(tests_cmake_text, "test_network_only_profile.cpp", tests_cmake)
    require(tests_cmake_text, "runtime_model", tests_cmake)
    for fragment in (
        "maxHandlersPerDispatch == 2",
        "continuationRejections == 1",
        "handlerOverruns == 1",
        "outputOverloads == 1",
        "crossDomainHandoffs == 0",
        "future_status::ready",
        "reentrantProfile->stop()",
        "repliesAccepted == 0",
    ):
        require(contract_text, fragment, contract)

    require(runtime_intent_text, "SingleLoopInlineEvent", runtime_intent)
    require(profile_intent_text, "zero cross-domain handoffs", profile_intent)
    require(profile_intent_text, "must not block", profile_intent)


if __name__ == "__main__":
    main()
