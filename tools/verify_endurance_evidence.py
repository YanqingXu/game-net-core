from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.production_endurance.v1"
PROFILES = {
    "abrupt_peer_reset",
    "callback_exception",
    "output_overload",
    "healthy_recovery",
    "forced_shutdown",
}
DURATIONS = {"candidate-24h": 86_400, "release-72h": 259_200}
LINUX_MAX_RSS_BYTES = 512 * 1024 * 1024
LINUX_MAX_RSS_GROWTH_BYTES = 64 * 1024 * 1024


class EvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_and_verify(
    path: Path,
    mode: str,
    candidate_sha: str,
    platform: str,
    backend: str,
) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read endurance evidence {path}: {error}") from error
    require(isinstance(document, dict), "endurance evidence must be an object")
    require(document.get("schema") == SCHEMA, "unexpected endurance evidence schema")
    require(document.get("status") == "success", "endurance evidence did not succeed")
    require(document.get("mode") == mode, "endurance mode mismatch")
    require(document.get("candidate_sha") == candidate_sha, "endurance candidate SHA mismatch")
    require(document.get("platform") == platform, "endurance platform mismatch")
    require(document.get("backend") == backend, "endurance backend mismatch")
    require(re.fullmatch(r"[0-9a-f]{40}", candidate_sha) is not None, "invalid expected candidate SHA")
    target = DURATIONS[mode]
    require(document.get("target_duration_seconds") == target, "endurance target duration drift")
    elapsed = document.get("child_elapsed_milliseconds")
    require(isinstance(elapsed, int) and elapsed >= target * 1000, "endurance duration shortfall")
    cycles = document.get("completed_cycles")
    require(isinstance(cycles, int) and cycles > 0, "endurance evidence has no completed cycles")
    profiles = document.get("profiles")
    require(isinstance(profiles, dict) and set(profiles) == PROFILES, "endurance profile inventory drift")
    require(all(profiles[name] == cycles for name in PROFILES), "endurance profile counts drifted")
    require(document.get("process_exit_code") == 0, "endurance process exit code was not zero")

    memory = document.get("memory")
    require(isinstance(memory, dict), "endurance memory evidence is missing")
    if platform == "linux":
        require(memory.get("supported") is True, "Linux RSS evidence is not supported")
        require(memory.get("samples") == cycles, "Linux RSS sample count drift")
        for field in (
            "first_rss_bytes",
            "last_rss_bytes",
            "minimum_rss_bytes",
            "maximum_rss_bytes",
            "rss_growth_bytes",
        ):
            require(isinstance(memory.get(field), int) and memory[field] >= 0, f"invalid {field}")
        require(memory.get("max_rss_budget_bytes") == LINUX_MAX_RSS_BYTES, "Linux RSS budget drift")
        require(
            memory.get("max_rss_growth_budget_bytes") == LINUX_MAX_RSS_GROWTH_BYTES,
            "Linux RSS growth budget drift",
        )
        require(memory["maximum_rss_bytes"] <= LINUX_MAX_RSS_BYTES, "Linux maximum RSS exceeded")
        require(memory["rss_growth_bytes"] <= LINUX_MAX_RSS_GROWTH_BYTES, "Linux RSS growth exceeded")

    test = document.get("test")
    require(isinstance(test, dict), "endurance test identity is missing")
    require(test.get("name") == "integration.resilience.test_fault_injection", "endurance test name drift")
    labels = test.get("labels")
    require(
        isinstance(labels, list)
        and {"endurance", "fault_injection", "threading", "lifecycle"} <= set(labels),
        "endurance test labels are incomplete",
    )
    require(
        re.fullmatch(r"[0-9a-f]{64}", str(test.get("executable_sha256", ""))) is not None,
        "endurance executable hash is invalid",
    )

    log = document.get("log")
    require(isinstance(log, dict), "endurance log identity is missing")
    log_name = log.get("file")
    require(isinstance(log_name, str) and Path(log_name).name == log_name, "unsafe endurance log path")
    log_path = path.parent / log_name
    require(log_path.is_file(), "retained endurance log is missing")
    require(log.get("bytes") == log_path.stat().st_size, "endurance log size drift")
    require(log.get("sha256") == sha256_file(log_path), "endurance log hash drift")
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify one retained GameNet endurance result")
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--mode", choices=tuple(DURATIONS), required=True)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    arguments = parser.parse_args()
    try:
        evidence = load_and_verify(
            arguments.evidence,
            arguments.mode,
            arguments.candidate_sha,
            arguments.platform,
            arguments.backend,
        )
    except EvidenceError as error:
        print(f"endurance evidence verification failed: {error}", file=sys.stderr)
        return 1
    print(
        f"verified {evidence['mode']} endurance evidence: "
        f"cycles={evidence['completed_cycles']} sha={evidence['candidate_sha']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
