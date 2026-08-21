#!/usr/bin/env python3
"""Run the fixed, warm, interleaved IOE-X10 listener protocol."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any

import validate_ioe_x10_listener_comparison as validator


SCHEMA = "gamenet.ioe_x10_listener_evidence.v1"
SAMPLES_PER_BACKEND = 5
WARMUPS_PER_BACKEND = 1
MEDIAN_METRICS = (
    "connects_per_second",
    "round_trips_per_second",
    "throughput_mib_per_second",
    "p50_latency_us",
    "p99_latency_us",
    "p999_latency_us",
    "fd_high_water",
    "rss_high_water_bytes",
    "max_pending_send_bytes",
    "max_capacity_recovery_milliseconds",
    "listener_shutdown_milliseconds",
    "server_shutdown_milliseconds",
)


class RunnerError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RunnerError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_affinity(text: str) -> set[int]:
    cpus: set[int] = set()
    for part in text.split(","):
        part = part.strip()
        require(bool(part), "empty CPU-affinity component")
        if "-" in part:
            first_text, last_text = part.split("-", 1)
            first, last = int(first_text), int(last_text)
            require(0 <= first <= last, "invalid CPU-affinity range")
            cpus.update(range(first, last + 1))
        else:
            value = int(part)
            require(value >= 0, "CPU-affinity values must be non-negative")
            cpus.add(value)
    require(bool(cpus), "CPU affinity must not be empty")
    return cpus


def cpu_model() -> str:
    for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
        if line.lower().startswith("model name"):
            return line.split(":", 1)[1].strip()
    raise RunnerError("CPU model is unavailable")


def compiler_version() -> str:
    result = subprocess.run(
        ["c++", "--version"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return result.stdout.splitlines()[0]


def git_commit(repo: Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return result.stdout.strip()


def run_sample(binary: Path, backend: str, label: str) -> dict[str, Any]:
    result = subprocess.run(
        [str(binary), backend],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    require(
        result.returncode == 0,
        f"{label} failed with {result.returncode}: {result.stderr.strip()}",
    )
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RunnerError(f"{label} emitted invalid JSON: {error}") from error
    return validator.validate_document(
        document,
        expected_backend=backend,
        require_release=True,
    )


def write_document(path: Path, document: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def medians(documents: list[dict[str, Any]]) -> dict[str, float]:
    return {
        name: float(
            statistics.median(document["measurements"][name] for document in documents)
        )
        for name in MEDIAN_METRICS
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--affinity", required=True, help="for example: 0-1")
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    args = parser.parse_args()

    require(sys.platform.startswith("linux"), "IOE-X10 requires Linux")
    binary = args.binary.resolve()
    repo = args.repo.resolve()
    output_dir = args.output_dir.resolve()
    require(binary.is_file() and os.access(binary, os.X_OK), "binary is not executable")
    require(repo.is_dir(), "repository path is invalid")
    output_dir.mkdir(parents=True, exist_ok=True)
    require(not any(output_dir.iterdir()), "output directory must be empty")

    requested_affinity = parse_affinity(args.affinity)
    allowed_affinity = os.sched_getaffinity(0)
    require(requested_affinity <= allowed_affinity, "requested affinity is unavailable")
    os.sched_setaffinity(0, requested_affinity)
    actual_affinity = sorted(os.sched_getaffinity(0))
    require(actual_affinity == sorted(requested_affinity), "affinity did not apply")

    failure: str | None = None
    samples: dict[str, list[dict[str, Any]]] = {"epoll": [], "io_uring": []}
    sample_entries: dict[str, list[dict[str, str]]] = {
        "epoll": [],
        "io_uring": [],
    }
    run_order: list[dict[str, Any]] = []
    try:
        for backend in ("epoll", "io_uring"):
            run_sample(binary, backend, f"{backend} warm-up")
            run_order.append({"kind": "warmup", "backend": backend, "repetition": 1})

        for repetition in range(1, SAMPLES_PER_BACKEND + 1):
            order = ("epoll", "io_uring") if repetition % 2 == 1 else ("io_uring", "epoll")
            for backend in order:
                document = run_sample(
                    binary,
                    backend,
                    f"{backend} formal sample {repetition}",
                )
                relative = Path(f"sample-{backend}-{repetition}.json")
                path = output_dir / relative
                write_document(path, document)
                samples[backend].append(document)
                sample_entries[backend].append(
                    {"path": relative.as_posix(), "sha256": sha256_file(path)}
                )
                run_order.append(
                    {"kind": "formal", "backend": backend, "repetition": repetition}
                )
    except Exception as error:  # Preserve a fail-closed decision artifact.
        failure = str(error)

    complete = all(len(documents) == SAMPLES_PER_BACKEND for documents in samples.values())
    decision = "PROMOTE" if failure is None and complete else "DEFER"
    median_values = {
        backend: medians(documents) if documents else {}
        for backend, documents in samples.items()
    }
    ratios: dict[str, float] = {}
    if complete:
        ratios = {
            name: median_values["io_uring"][name] / median_values["epoll"][name]
            for name in (
                "connects_per_second",
                "round_trips_per_second",
                "throughput_mib_per_second",
                "p50_latency_us",
                "p99_latency_us",
                "p999_latency_us",
                "rss_high_water_bytes",
                "server_shutdown_milliseconds",
            )
            if median_values["epoll"][name] != 0
        }

    evidence = {
        "schema": SCHEMA,
        "decision": decision,
        "decision_scope": "later source-private io_uring integration shaping only",
        "failure": failure,
        "metadata": {
            "commit": git_commit(repo),
            "machine": platform.node(),
            "cpu": cpu_model(),
            "kernel": platform.release(),
            "platform": platform.platform(),
            "compiler": compiler_version(),
            "build_type": "Release",
            "affinity": actual_affinity,
        },
        "protocol": validator.PARAMETERS,
        "sampling": {
            "warmups_per_backend": WARMUPS_PER_BACKEND,
            "formal_samples_per_backend": SAMPLES_PER_BACKEND,
            "interleaved": True,
            "alternating_first_backend": True,
            "run_order": run_order,
        },
        "samples": sample_entries,
        "medians": median_values,
        "io_uring_to_epoll_median_ratios": ratios,
        "checks": {
            "fixed_protocol": failure is None and complete,
            "release_build": failure is None and complete,
            "same_machine_compiler_affinity": failure is None and complete,
            "one_warmup_per_backend": len(
                [item for item in run_order if item["kind"] == "warmup"]
            ) == 2,
            "five_samples_per_backend": complete,
            "alternating_interleave": failure is None and complete,
            "all_samples_valid": failure is None and complete,
            "zero_final_residue": failure is None and complete,
            "stable_api_or_backend_selector_changed": False,
        },
    }
    manifest = output_dir / "evidence.json"
    write_document(manifest, evidence)
    print(manifest)
    if decision != "PROMOTE":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
