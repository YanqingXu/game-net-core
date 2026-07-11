from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing PacketFramer fuzz contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    top_level = repo_root / "CMakeLists.txt"
    fuzz_cmake = repo_root / "tests" / "fuzz" / "CMakeLists.txt"
    harness = repo_root / "tests" / "fuzz" / "protocol" / "fuzz_packet_framer.cpp"
    corpus_generator = repo_root / "tests" / "fuzz" / "generate_packet_framer_corpus.py"
    corpus_dir = repo_root / "tests" / "fuzz" / "corpus" / "packet_framer"
    dictionary = repo_root / "tests" / "fuzz" / "packet_framer.dict"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    evidence_verifier = repo_root / "tools" / "verify_ci_evidence_set.py"

    top_text = top_level.read_text(encoding="utf-8")
    fuzz_text = fuzz_cmake.read_text(encoding="utf-8")
    harness_text = harness.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    verifier_text = evidence_verifier.read_text(encoding="utf-8")

    require(top_text, 'option(GAMENET_BUILD_FUZZING "Build optional libFuzzer targets" OFF)', top_level)
    require(top_text, "add_subdirectory(tests/fuzz)", top_level)
    require(fuzz_text, "gamenet_fuzz_packet_framer", fuzz_cmake)
    require(fuzz_text, "-fsanitize=fuzzer-no-link,address,undefined", fuzz_cmake)
    require(fuzz_text, "-fsanitize=fuzzer,address,undefined", fuzz_cmake)
    require(harness_text, 'extern "C" int LLVMFuzzerTestOneInput', harness)
    require(harness_text, "needsContinuation", harness)
    assert corpus_generator.exists(), f"missing deterministic corpus generator: {corpus_generator}"
    generator_text = corpus_generator.read_text(encoding="utf-8")
    require(generator_text, '"valid-empty-frame": b"\\x00\\x00\\x00\\x00"', corpus_generator)
    require(generator_text, '"sticky-valid-frames"', corpus_generator)
    require(generator_text, '"oversized-length"', corpus_generator)
    expected_seeds = {
        "valid-empty-frame": b"\x00\x00\x00\x00",
        "valid-one-byte-frame": b"\x00\x00\x00\x01A",
        "partial-prefix": b"\x00\x00\x00",
        "oversized-length": b"\xff\xff\xff\xff",
        "sticky-valid-frames": b"\x00\x00\x00\x01A\x00\x00\x00\x02BC",
        "valid-64-byte-frame": b"\x00\x00\x00\x40" + b"x" * 64,
    }
    assert corpus_dir.is_dir(), f"missing PacketFramer corpus directory: {corpus_dir}"
    assert {path.name for path in corpus_dir.iterdir()} == set(expected_seeds), (
        "PacketFramer corpus must contain only the deterministic binary seeds"
    )
    for name, expected in expected_seeds.items():
        assert (corpus_dir / name).read_bytes() == expected, f"stale fuzz corpus seed: {name}"
    assert dictionary.is_file(), f"missing PacketFramer fuzz dictionary: {dictionary}"
    assert len([line for line in dictionary.read_text(encoding="utf-8").splitlines() if line]) >= 5
    require(workflow_text, "-DGAMENET_BUILD_FUZZING=ON", workflow)
    require(workflow_text, "--target gamenet_fuzz_packet_framer", workflow)
    require(workflow_text, "tests/fuzz/corpus/packet_framer", workflow)
    require(
        workflow_text,
        "cmake -E copy_if_different tests/fuzz/packet_framer.dict ci-evidence/fuzz/packet_framer.dict",
        workflow,
    )
    require(workflow_text, "ci-evidence/fuzz/corpus", workflow)
    require(workflow_text, "-dict=ci-evidence/fuzz/packet_framer.dict", workflow)
    require(workflow_text, "-artifact_prefix=ci-evidence/fuzz/artifacts/", workflow)
    require(workflow_text, "-seed=424242", workflow)
    require(workflow_text, "-print_final_stats=1", workflow)
    require(workflow_text, "-runs=1000", workflow)
    require(workflow_text, "-max_len=8192", workflow)
    assert "-max_total_time" not in workflow_text
    require(workflow_text, "-timeout=2", workflow)
    require(verifier_text, "stat::number_of_executed_units", evidence_verifier)
    require(verifier_text, "ASan libFuzzer execution count mismatch", evidence_verifier)


if __name__ == "__main__":
    main()
