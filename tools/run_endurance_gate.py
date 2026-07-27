from __future__ import annotations

import argparse
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


EVIDENCE_SCHEMA = "gamenet.production_endurance.v1"
HEARTBEAT_SCHEMA = "gamenet.fault_injection_heartbeat.v1"
SUMMARY_SCHEMA = "gamenet.fault_injection_summary.v1"
TEST_NAME = "integration.resilience.test_fault_injection"
PROFILES = (
    "abrupt_peer_reset",
    "callback_exception",
    "output_overload",
    "healthy_recovery",
    "forced_shutdown",
)
PRODUCTION_DURATIONS = {
    "candidate-24h": 24 * 60 * 60,
    "release-72h": 72 * 60 * 60,
}
LINUX_MAX_RSS_BYTES = 512 * 1024 * 1024
LINUX_MAX_RSS_GROWTH_BYTES = 64 * 1024 * 1024


class EnduranceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EnduranceError(message)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def linux_resident_bytes(process_id: int) -> int:
    status = Path(f"/proc/{process_id}/status")
    try:
        for line in status.read_text(encoding="utf-8").splitlines():
            if line.startswith("VmRSS:"):
                fields = line.split()
                require(len(fields) == 3 and fields[2] == "kB", "unexpected Linux VmRSS format")
                return int(fields[1]) * 1024
    except (OSError, UnicodeError, ValueError) as error:
        raise EnduranceError(f"cannot sample Linux child RSS: {error}") from error
    raise EnduranceError("Linux child RSS is missing")


def atomic_json(path: Path, document: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def exact_test_pattern() -> str:
    return "^" + re.escape(TEST_NAME) + "$"


def resolve_test(test_dir: Path, configuration: str | None) -> tuple[Path, Path, list[str]]:
    ctest = shutil.which("ctest")
    require(ctest is not None, "ctest is not available on PATH")
    command = [ctest, "--test-dir", str(test_dir)]
    if configuration:
        command.extend(["-C", configuration])
    command.extend(["--show-only=json-v1", "-R", exact_test_pattern()])
    result = subprocess.run(command, capture_output=True, text=True, errors="replace", check=False)
    require(result.returncode == 0, f"cannot query CTest inventory: {result.stderr.strip()}")
    try:
        inventory = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise EnduranceError(f"CTest inventory is not JSON: {error}") from error
    tests = inventory.get("tests")
    require(isinstance(tests, list) and len(tests) == 1, "fault-injection CTest must resolve exactly once")
    test = tests[0]
    require(test.get("name") == TEST_NAME, "resolved the wrong CTest")
    test_command = test.get("command")
    require(
        isinstance(test_command, list)
        and len(test_command) == 1
        and isinstance(test_command[0], str),
        "fault-injection CTest must resolve to one executable without arguments",
    )
    executable = Path(test_command[0]).resolve()
    require(executable.is_file(), f"fault-injection executable is missing: {executable}")

    properties = {
        item.get("name"): item.get("value")
        for item in test.get("properties", [])
        if isinstance(item, dict)
    }
    labels = properties.get("LABELS")
    require(isinstance(labels, list), "fault-injection CTest has no labels")
    require(
        {"endurance", "fault_injection", "threading", "lifecycle"} <= set(labels),
        "fault-injection CTest labels are incomplete",
    )
    working_directory = Path(properties.get("WORKING_DIRECTORY", test_dir)).resolve()
    require(working_directory.is_dir(), "fault-injection working directory is missing")
    return executable, working_directory, sorted(labels)


def duration_for(mode: str, override: int | None) -> int:
    if mode in PRODUCTION_DURATIONS:
        require(override is None, "production modes reject duration overrides")
        return PRODUCTION_DURATIONS[mode]
    require(mode == "smoke", f"unsupported endurance mode: {mode}")
    require(override is not None and 1 <= override <= 60, "smoke duration must be 1..60 seconds")
    return override


def validate_heartbeat(document: Any, expected_cycle: int, previous_elapsed: int) -> tuple[int, int]:
    require(isinstance(document, dict), "heartbeat must be a JSON object")
    require(document.get("schema") == HEARTBEAT_SCHEMA, "unexpected heartbeat schema")
    require(document.get("result") == "pass", "fault-injection heartbeat did not pass")
    cycle = document.get("cycle")
    elapsed = document.get("elapsed_milliseconds")
    require(cycle == expected_cycle, f"heartbeat cycle drift: expected {expected_cycle}, got {cycle}")
    require(isinstance(elapsed, int) and elapsed >= previous_elapsed, "heartbeat elapsed time regressed")
    profiles = document.get("profiles")
    require(isinstance(profiles, dict), "heartbeat profiles are missing")
    require(set(profiles) == set(PROFILES), "heartbeat profile inventory drift")
    require(all(profiles[name] == cycle for name in PROFILES), "heartbeat profile counts drifted")
    return cycle, elapsed


def validate_summary(document: Any, cycle: int, elapsed: int, duration_seconds: int) -> int:
    require(isinstance(document, dict), "summary must be a JSON object")
    require(document.get("schema") == SUMMARY_SCHEMA, "unexpected fault-injection summary schema")
    require(document.get("result") == "pass", "fault-injection summary did not pass")
    require(document.get("cycle") == cycle and cycle > 0, "summary cycle does not match heartbeats")
    summary_elapsed = document.get("elapsed_milliseconds")
    require(
        isinstance(summary_elapsed, int) and summary_elapsed >= elapsed,
        "summary elapsed time regressed",
    )
    require(
        summary_elapsed >= duration_seconds * 1000,
        "fault-injection process exited before the required duration",
    )
    profiles = document.get("profiles")
    require(isinstance(profiles, dict) and set(profiles) == set(PROFILES), "summary profiles drifted")
    require(all(profiles[name] == cycle for name in PROFILES), "summary profile counts drifted")
    return summary_elapsed


def checkpoint_document(
    *,
    mode: str,
    candidate_sha: str,
    platform: str,
    backend: str,
    target_seconds: int,
    started_at: str,
    status: str,
    cycle: int,
    child_elapsed_milliseconds: int,
    memory: dict[str, Any] | None = None,
    message: str | None = None,
) -> dict[str, Any]:
    document: dict[str, Any] = {
        "schema": EVIDENCE_SCHEMA,
        "generated_at_utc": utc_now(),
        "status": status,
        "mode": mode,
        "candidate_sha": candidate_sha,
        "platform": platform,
        "backend": backend,
        "target_duration_seconds": target_seconds,
        "started_at_utc": started_at,
        "completed_cycles": cycle,
        "child_elapsed_milliseconds": child_elapsed_milliseconds,
        "profiles": {name: cycle for name in PROFILES},
    }
    if message is not None:
        document["message"] = message
    if memory is not None:
        document["memory"] = memory
    return document


def run_endurance(arguments: argparse.Namespace) -> dict[str, Any]:
    require(re.fullmatch(r"[0-9a-f]{40}", arguments.candidate_sha) is not None, "candidate SHA must be 40 lowercase hex characters")
    require(
        (arguments.platform, arguments.backend) in {("linux", "epoll"), ("windows", "iocp")},
        "platform/backend pairing is unsupported",
    )
    require(0 <= arguments.interval_milliseconds <= 60_000, "interval must be 0..60000 milliseconds")
    require(arguments.heartbeat_timeout_seconds > 0, "heartbeat timeout must be positive")
    target_seconds = duration_for(arguments.mode, arguments.duration_seconds)
    require(not arguments.output_root.exists(), "output root already exists; refusing to overwrite evidence")
    arguments.output_root.mkdir(parents=True)

    executable, working_directory, labels = resolve_test(arguments.test_dir, arguments.configuration)
    log_path = arguments.output_root / "fault-injection.log"
    checkpoint_path = arguments.output_root / "checkpoint.json"
    result_path = arguments.output_root / "result.json"
    started_at = utc_now()
    started_monotonic = time.monotonic()
    cycle = 0
    child_elapsed = 0
    summary_elapsed: int | None = None
    process: subprocess.Popen[str] | None = None
    failure: str | None = None
    exit_code: int | None = None
    rss_samples = 0
    first_rss: int | None = None
    last_rss: int | None = None
    minimum_rss: int | None = None
    maximum_rss: int | None = None

    def memory_document() -> dict[str, Any]:
        if arguments.platform != "linux":
            return {"supported": False, "samples": 0}
        growth = 0 if first_rss is None or last_rss is None else max(0, last_rss - first_rss)
        return {
            "supported": True,
            "samples": rss_samples,
            "first_rss_bytes": first_rss,
            "last_rss_bytes": last_rss,
            "minimum_rss_bytes": minimum_rss,
            "maximum_rss_bytes": maximum_rss,
            "rss_growth_bytes": growth,
            "max_rss_budget_bytes": LINUX_MAX_RSS_BYTES,
            "max_rss_growth_budget_bytes": LINUX_MAX_RSS_GROWTH_BYTES,
        }

    atomic_json(
        checkpoint_path,
        checkpoint_document(
            mode=arguments.mode,
            candidate_sha=arguments.candidate_sha,
            platform=arguments.platform,
            backend=arguments.backend,
            target_seconds=target_seconds,
            started_at=started_at,
            status="starting",
            cycle=cycle,
            child_elapsed_milliseconds=child_elapsed,
            memory=memory_document(),
        ),
    )

    environment = os.environ.copy()
    environment["GAMENET_ENDURANCE_SECONDS"] = str(target_seconds)
    environment["GAMENET_ENDURANCE_INTERVAL_MS"] = str(arguments.interval_milliseconds)
    environment["GAMENET_ENDURANCE_OBSERVATION_ACK"] = "1"
    lines: queue.Queue[str | None] = queue.Queue()

    try:
        process = subprocess.Popen(
            [str(executable)],
            cwd=working_directory,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.PIPE,
            text=True,
            errors="replace",
            bufsize=1,
        )
        require(process.stdout is not None, "failed to capture fault-injection output")
        require(process.stdin is not None, "failed to open fault-injection acknowledgment pipe")

        def read_output() -> None:
            assert process is not None and process.stdout is not None
            for line in process.stdout:
                lines.put(line)
            lines.put(None)

        reader = threading.Thread(target=read_output, name="endurance-output-reader", daemon=True)
        reader.start()
        last_heartbeat = time.monotonic()
        summary_seen = False

        with log_path.open("w", encoding="utf-8", newline="\n") as log:
            while True:
                silence_remaining = arguments.heartbeat_timeout_seconds - (
                    time.monotonic() - last_heartbeat
                )
                require(silence_remaining > 0, "fault-injection heartbeat timed out")
                try:
                    line = lines.get(timeout=min(1.0, silence_remaining))
                except queue.Empty:
                    if process.poll() is not None and lines.empty():
                        break
                    continue
                if line is None:
                    break
                log.write(line)
                log.flush()
                stripped = line.strip()
                if not stripped.startswith("{"):
                    continue
                try:
                    event = json.loads(stripped)
                except json.JSONDecodeError as error:
                    raise EnduranceError(f"malformed JSON event: {error}") from error
                schema = event.get("schema") if isinstance(event, dict) else None
                if schema == HEARTBEAT_SCHEMA:
                    require(not summary_seen, "heartbeat appeared after summary")
                    cycle, child_elapsed = validate_heartbeat(event, cycle + 1, child_elapsed)
                    if arguments.platform == "linux":
                        sampled_rss = linux_resident_bytes(process.pid)
                        rss_samples += 1
                        first_rss = sampled_rss if first_rss is None else first_rss
                        last_rss = sampled_rss
                        minimum_rss = sampled_rss if minimum_rss is None else min(minimum_rss, sampled_rss)
                        maximum_rss = sampled_rss if maximum_rss is None else max(maximum_rss, sampled_rss)
                        require(maximum_rss <= LINUX_MAX_RSS_BYTES, "Linux child RSS exceeded its budget")
                        require(
                            last_rss - first_rss <= LINUX_MAX_RSS_GROWTH_BYTES,
                            "Linux child RSS growth exceeded its budget",
                        )
                    try:
                        process.stdin.write("observed\n")
                        process.stdin.flush()
                    except (BrokenPipeError, OSError) as error:
                        raise EnduranceError(
                            "fault-injection process exited before heartbeat observation acknowledgment"
                        ) from error
                    last_heartbeat = time.monotonic()
                    atomic_json(
                        checkpoint_path,
                        checkpoint_document(
                            mode=arguments.mode,
                            candidate_sha=arguments.candidate_sha,
                            platform=arguments.platform,
                            backend=arguments.backend,
                            target_seconds=target_seconds,
                            started_at=started_at,
                            status="running",
                            cycle=cycle,
                            child_elapsed_milliseconds=child_elapsed,
                            memory=memory_document(),
                        ),
                    )
                elif schema == SUMMARY_SCHEMA:
                    require(not summary_seen, "duplicate fault-injection summary")
                    summary_elapsed = validate_summary(event, cycle, child_elapsed, target_seconds)
                    summary_seen = True
                    last_heartbeat = time.monotonic()
                else:
                    raise EnduranceError(f"unexpected structured event schema: {schema!r}")

        exit_code = process.wait(timeout=10)
        reader.join(timeout=2)
        require(exit_code == 0, f"fault-injection executable exited with {exit_code}")
        require(summary_seen and summary_elapsed is not None, "fault-injection summary is missing")
        require(cycle > 0, "fault-injection process completed no cycles")
        require(
            time.monotonic() - started_monotonic >= target_seconds,
            "supervisor observed a duration shortfall",
        )
    except (EnduranceError, OSError, subprocess.SubprocessError) as error:
        failure = str(error)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if process is not None:
            exit_code = process.returncode

    supervisor_seconds = time.monotonic() - started_monotonic
    status = "success" if failure is None else "failure"
    final = checkpoint_document(
        mode=arguments.mode,
        candidate_sha=arguments.candidate_sha,
        platform=arguments.platform,
        backend=arguments.backend,
        target_seconds=target_seconds,
        started_at=started_at,
        status=status,
        cycle=cycle,
        child_elapsed_milliseconds=summary_elapsed or child_elapsed,
        memory=memory_document(),
        message=failure,
    )
    final.update(
        {
            "finished_at_utc": utc_now(),
            "supervisor_elapsed_seconds": supervisor_seconds,
            "process_exit_code": exit_code,
            "test": {
                "name": TEST_NAME,
                "labels": labels,
                "executable": str(executable),
                "executable_sha256": sha256_file(executable),
                "working_directory": str(working_directory),
            },
        }
    )
    if log_path.exists():
        final["log"] = {
            "file": log_path.name,
            "bytes": log_path.stat().st_size,
            "sha256": sha256_file(log_path),
        }
    atomic_json(checkpoint_path, final)
    atomic_json(result_path, final)
    if failure is not None:
        raise EnduranceError(failure)
    return final


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the uninterrupted GameNet production endurance gate")
    parser.add_argument("--test-dir", type=Path, required=True)
    parser.add_argument("--configuration")
    parser.add_argument("--mode", choices=("smoke", *PRODUCTION_DURATIONS), required=True)
    parser.add_argument("--duration-seconds", type=int)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--interval-milliseconds", type=int, default=1000)
    parser.add_argument("--heartbeat-timeout-seconds", type=int, default=120)
    parser.add_argument("--output-root", type=Path, required=True)
    arguments = parser.parse_args()

    try:
        result = run_endurance(arguments)
    except EnduranceError as error:
        print(f"production endurance gate failed: {error}", file=sys.stderr)
        return 1
    print(
        f"production endurance gate passed: mode={result['mode']} "
        f"cycles={result['completed_cycles']} duration={result['child_elapsed_milliseconds']}ms"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
