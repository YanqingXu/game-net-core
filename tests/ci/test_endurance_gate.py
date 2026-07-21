from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


SHA = "a" * 40
HOST_PLATFORM = "windows" if os.name == "nt" else "linux"
HOST_BACKEND = "iocp" if os.name == "nt" else "epoll"
PROFILES = (
    "abrupt_peer_reset",
    "callback_exception",
    "output_overload",
    "healthy_recovery",
    "forced_shutdown",
)


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing endurance contract fragment in {source}: {needle}"


def run(command: list[str], *, env: dict[str, str] | None = None, expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        errors="replace",
        env=env,
        timeout=30,
        check=False,
    )
    assert result.returncode == expected, (
        f"unexpected command result {result.returncode}, expected {expected}: {' '.join(command)}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    return result


def write_fixture_project(root: Path) -> Path:
    source = root / "source"
    build = root / "build"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.20)
project(EnduranceFixture LANGUAGES CXX)
enable_testing()
add_executable(fault fake_fault.cpp)
add_test(NAME integration.resilience.test_fault_injection COMMAND fault)
set_tests_properties(integration.resilience.test_fault_injection PROPERTIES
  LABELS "endurance;fault_injection;threading;lifecycle"
)
""",
        encoding="utf-8",
    )
    (source / "fake_fault.cpp").write_text(
        r'''#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main() {
    const int seconds = std::stoi(std::getenv("GAMENET_ENDURANCE_SECONDS"));
    const std::string mode = std::getenv("GAMENET_FAKE_MODE") == nullptr
        ? "pass" : std::getenv("GAMENET_FAKE_MODE");
    if (mode == "malformed") {
        std::cout << "{malformed\n" << std::flush;
        return 0;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const auto profiles = "\"profiles\":{\"abrupt_peer_reset\":1,"
        "\"callback_exception\":1,\"output_overload\":1,"
        "\"healthy_recovery\":1,\"forced_shutdown\":1}";
    std::cout << "{\"schema\":\"gamenet.fault_injection_heartbeat.v1\","
        "\"result\":\"pass\",\"cycle\":1,"
        "\"elapsed_milliseconds\":" << seconds * 1000 << ',' << profiles << "}\n"
        << std::flush;
    if (mode == "failed") {
        return 7;
    }
    std::cout << "{\"schema\":\"gamenet.fault_injection_summary.v1\","
        "\"result\":\"pass\",\"cycle\":1,"
        "\"elapsed_milliseconds\":" << seconds * 1000 << ',' << profiles << "}\n"
        << std::flush;
    return 0;
}
''',
        encoding="utf-8",
    )
    run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", str(build), "--config", "Release", "--parallel"])
    return build


def runner_command(repo_root: Path, build: Path, output: Path) -> list[str]:
    return [
        sys.executable,
        str(repo_root / "tools" / "run_endurance_gate.py"),
        "--test-dir",
        str(build),
        "--configuration",
        "Release",
        "--mode",
        "smoke",
        "--duration-seconds",
        "1",
        "--candidate-sha",
        SHA,
        "--platform",
        HOST_PLATFORM,
        "--backend",
        HOST_BACKEND,
        "--interval-milliseconds",
        "0",
        "--heartbeat-timeout-seconds",
        "5",
        "--output-root",
        str(output),
    ]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_synthetic_evidence(root: Path, mode: str, seconds: int) -> Path:
    root.mkdir()
    log = root / "fault-injection.log"
    log.write_text("retained synthetic verifier fixture\n", encoding="utf-8")
    evidence = {
        "schema": "gamenet.production_endurance.v1",
        "status": "success",
        "mode": mode,
        "candidate_sha": SHA,
        "platform": "linux",
        "backend": "epoll",
        "target_duration_seconds": seconds,
        "completed_cycles": 3,
        "child_elapsed_milliseconds": seconds * 1000,
        "profiles": {name: 3 for name in PROFILES},
        "process_exit_code": 0,
        "memory": {
            "supported": True,
            "samples": 3,
            "first_rss_bytes": 20 * 1024 * 1024,
            "last_rss_bytes": 21 * 1024 * 1024,
            "minimum_rss_bytes": 20 * 1024 * 1024,
            "maximum_rss_bytes": 22 * 1024 * 1024,
            "rss_growth_bytes": 1024 * 1024,
            "max_rss_budget_bytes": 512 * 1024 * 1024,
            "max_rss_growth_budget_bytes": 64 * 1024 * 1024,
        },
        "test": {
            "name": "integration.resilience.test_fault_injection",
            "labels": ["endurance", "fault_injection", "threading", "lifecycle"],
            "executable_sha256": "b" * 64,
        },
        "log": {"file": log.name, "bytes": log.stat().st_size, "sha256": sha256(log)},
    }
    result = root / "result.json"
    result.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = repo_root / ".github" / "workflows" / "long-soak.yml"
    workflow_text = workflow.read_text(encoding="utf-8")
    intent = repo_root / "intents" / "usecases" / "production_endurance.intent.md"
    intent_text = intent.read_text(encoding="utf-8")
    runner = repo_root / "tools" / "run_endurance_gate.py"
    runner_text = runner.read_text(encoding="utf-8")

    for fragment in (
        "candidate-24h",
        "release-72h",
        "86,400 seconds",
        "259,200",
        "tests/integration/resilience/test_fault_injection.cpp",
        "tests/ci/test_endurance_gate.py",
    ):
        require(intent_text, fragment, intent)
    for fragment in (
        '"candidate-24h": 24 * 60 * 60',
        '"release-72h": 72 * 60 * 60',
        '"supervisor observed a duration shortfall"',
        '"Linux child RSS growth exceeded its budget"',
        '"output root already exists; refusing to overwrite evidence"',
    ):
        require(runner_text, fragment, runner)
    for fragment in (
        "runs-on: [self-hosted, linux, x64, gamenet-endurance]",
        "timeout-minutes: 4620",
        "--expected-total 89",
        "--expect-label threading=64",
        "--expect-label fault_injection=1",
        "--expect-label endurance=1",
        "tools/run_endurance_gate.py",
        "tools/verify_endurance_evidence.py",
        "tools/verify_endurance_evidence_pair.py",
        "candidate_run_id",
        "actions/download-artifact@v4",
    ):
        require(workflow_text, fragment, workflow)
    production_invocation = workflow_text.split("- name: Run uninterrupted production endurance", 1)[1]
    production_invocation = production_invocation.split("- name: Verify current endurance result", 1)[0]
    assert "--duration-seconds" not in production_invocation, "production workflow must not shorten duration"

    with tempfile.TemporaryDirectory(prefix="gamenet-endurance-contract-") as directory:
        root = Path(directory)
        build = write_fixture_project(root)
        passed = root / "passed"
        run(runner_command(repo_root, build, passed))
        result = json.loads((passed / "result.json").read_text(encoding="utf-8"))
        assert result["status"] == "success"
        assert result["completed_cycles"] == 1
        assert result["child_elapsed_milliseconds"] >= 1000
        assert result["supervisor_elapsed_seconds"] >= 1

        malformed_env = os.environ.copy()
        malformed_env["GAMENET_FAKE_MODE"] = "malformed"
        malformed = root / "malformed"
        run(runner_command(repo_root, build, malformed), env=malformed_env, expected=1)
        malformed_result = json.loads((malformed / "result.json").read_text(encoding="utf-8"))
        assert malformed_result["status"] == "failure"
        assert "malformed JSON event" in malformed_result["message"]

        failed_env = os.environ.copy()
        failed_env["GAMENET_FAKE_MODE"] = "failed"
        failed = root / "failed"
        run(runner_command(repo_root, build, failed), env=failed_env, expected=1)
        failed_result = json.loads((failed / "result.json").read_text(encoding="utf-8"))
        assert failed_result["status"] == "failure"

        override = runner_command(repo_root, build, root / "override")
        mode_index = override.index("smoke")
        override[mode_index] = "candidate-24h"
        run(override, expected=1)

        candidate = write_synthetic_evidence(root / "candidate", "candidate-24h", 86_400)
        release = write_synthetic_evidence(root / "release", "release-72h", 259_200)
        verifier = repo_root / "tools" / "verify_endurance_evidence.py"
        pair_verifier = repo_root / "tools" / "verify_endurance_evidence_pair.py"
        run([
            sys.executable,
            str(verifier),
            "--evidence",
            str(candidate),
            "--mode",
            "candidate-24h",
            "--candidate-sha",
            SHA,
            "--platform",
            "linux",
            "--backend",
            "epoll",
        ])
        pair_output = root / "pair.json"
        run([
            sys.executable,
            str(pair_verifier),
            "--candidate-evidence",
            str(candidate),
            "--release-evidence",
            str(release),
            "--candidate-sha",
            SHA,
            "--platform",
            "linux",
            "--backend",
            "epoll",
            "--output",
            str(pair_output),
        ])
        assert json.loads(pair_output.read_text(encoding="utf-8"))["status"] == "success"

        (release.parent / "fault-injection.log").write_text("tampered\n", encoding="utf-8")
        run([
            sys.executable,
            str(verifier),
            "--evidence",
            str(release),
            "--mode",
            "release-72h",
            "--candidate-sha",
            SHA,
            "--platform",
            "linux",
            "--backend",
            "epoll",
        ], expected=1)

    print("production endurance runner, evidence, workflow, and failure fixtures verified")


if __name__ == "__main__":
    main()
