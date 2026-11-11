# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

import subprocess
import sys
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
    hp1_harness = repo_root / "tests/fuzz/prestudy/fuzz_packet_framer_view.cpp"
    hp1_workflow = repo_root / ".github/workflows/hp1-framing-fuzz.yml"
    hp1_gate = "if(TARGET gamenet_hp1_packet_framer_view)"
    require(fuzz_text, hp1_gate, fuzz_cmake)
    hp1_block = fuzz_text.split(hp1_gate, 1)[1].split("endif()", 1)[0]
    require(hp1_block, "add_executable(gamenet_fuzz_packet_framer_view", fuzz_cmake)
    require(hp1_block, "-fsanitize=fuzzer,address,undefined", fuzz_cmake)
    assert "install(" not in hp1_block and "export(" not in hp1_block
    hp1_text = hp1_harness.read_text(encoding="utf-8")
    for fragment in (
        "legacy_.push(chunk)", "candidate_.visitFrames(readable",
        "verifyRecovery", "view.retain()",
    ):
        require(hp1_text, fragment, hp1_harness)
    hp1_workflow_text = hp1_workflow.read_text(encoding="utf-8")
    for fragment in (
        "-DGAMENET_BUILD_BENCHMARKS=ON", "-DGAMENET_BUILD_FUZZING=ON",
        "-DGAMENET_ENABLE_ASAN_UBSAN=ON", "-runs=20000", "-max_len=8192",
        "-timeout=2", "-rss_limit_mb=512", "--no-tests=error",
        "actions/upload-artifact@v4", "if: always()",
    ):
        require(hp1_workflow_text, fragment, hp1_workflow)
    subprocess.run([sys.executable, str(corpus_generator), "--check", "--hp1"], check=True)
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
