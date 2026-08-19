#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_core_benchmark import (  # noqa: E402
    CoreBenchmarkValidationError,
    validate_document as validate_core_document,
)


SCHEMA = "gamenet.performance_matrix.v1"


@dataclass(frozen=True)
class Scenario:
    group: str
    key: str
    reported_scenario: str
    schema: str
    arguments: tuple[str, ...]
    canonical: bool = False
    require_zero_churn_failures: bool = False


SCENARIOS = (
    Scenario("core", "echo-1-worker", "echo", "gamenet.core_benchmark.v2", (
        "--scenario", "echo", "--connections", "4", "--threads", "1", "--messages", "10000",
        "--payload", "256", "--settle-ms", "500", "--timeout-ms", "30000"), True),
    Scenario("core", "echo-2-workers", "echo", "gamenet.core_benchmark.v2", (
        "--scenario", "echo", "--connections", "4", "--threads", "2", "--messages", "10000",
        "--payload", "256", "--settle-ms", "500", "--timeout-ms", "30000"), True),
    Scenario("core", "echo-4-workers", "echo", "gamenet.core_benchmark.v2", (
        "--scenario", "echo", "--connections", "8", "--threads", "4", "--messages", "5000",
        "--payload", "256", "--settle-ms", "500", "--timeout-ms", "30000")),
    Scenario("core", "connections-256", "connections", "gamenet.core_benchmark.v2", (
        "--scenario", "connections", "--connections", "256", "--threads", "1",
        "--settle-ms", "1000", "--timeout-ms", "30000"), True),
    Scenario("core", "connections-1024", "connections", "gamenet.core_benchmark.v2", (
        "--scenario", "connections", "--connections", "1024", "--threads", "4",
        "--settle-ms", "1000", "--timeout-ms", "30000")),
    Scenario("core", "slow-client-4", "slow-client", "gamenet.core_benchmark.v2", (
        "--scenario", "slow-client", "--connections", "4", "--threads", "1", "--slow-bytes", "8388608",
        "--high-water", "65536", "--settle-ms", "1000", "--timeout-ms", "30000"), True),
    Scenario("core", "slow-client-16", "slow-client", "gamenet.core_benchmark.v2", (
        "--scenario", "slow-client", "--connections", "16", "--threads", "4", "--slow-bytes", "4194304",
        "--high-water", "65536", "--settle-ms", "1000", "--timeout-ms", "30000")),
    Scenario("phase4", "framing", "framing", "gamenet.phase4_benchmark.v1", (
        "--scenario", "framing", "--messages", "250000", "--payload", "256", "--threads", "1",
        "--batch", "64", "--fanout", "1", "--tick-us", "1000", "--timeout-ms", "30000"), True),
    Scenario("phase4", "logic-queue", "logic-queue", "gamenet.phase4_benchmark.v1", (
        "--scenario", "logic-queue", "--messages", "20000", "--payload", "64", "--threads", "4",
        "--batch", "512", "--fanout", "1", "--tick-us", "1000", "--timeout-ms", "30000"), True),
    Scenario("phase4", "logic-queue-heavy", "logic-queue", "gamenet.phase4_benchmark.v1", (
        "--scenario", "logic-queue", "--messages", "40000", "--payload", "64", "--threads", "8",
        "--batch", "1024", "--fanout", "1", "--tick-us", "1000", "--timeout-ms", "30000")),
    Scenario("phase4", "broadcast-fanout", "broadcast-fanout", "gamenet.phase4_benchmark.v1", (
        "--scenario", "broadcast-fanout", "--messages", "32", "--payload", "256", "--threads", "2",
        "--batch", "64", "--fanout", "1024", "--tick-us", "1000", "--timeout-ms", "30000"), True),
    Scenario("phase4", "broadcast-fanout-4096", "broadcast-fanout", "gamenet.phase4_benchmark.v1", (
        "--scenario", "broadcast-fanout", "--messages", "16", "--payload", "256", "--threads", "4",
        "--batch", "64", "--fanout", "4096", "--tick-us", "1000", "--timeout-ms", "30000")),
)

CAPACITY_SCENARIOS = (
    Scenario("core", "connections-1000", "connections", "gamenet.core_benchmark.v2", (
        "--scenario", "connections", "--connections", "1000", "--threads", "1",
        "--settle-ms", "2000", "--timeout-ms", "120000")),
    Scenario("core", "connections-10000", "connections", "gamenet.core_benchmark.v2", (
        "--scenario", "connections", "--connections", "10000", "--threads", "1",
        "--settle-ms", "2000", "--timeout-ms", "120000")),
    *tuple(
        Scenario(
            "core",
            f"echo-{payload}-{workers}-workers",
            "echo",
            "gamenet.core_benchmark.v2",
            (
                "--scenario", "echo",
                "--connections", "16",
                "--threads", str(workers),
                "--messages", "2000",
                "--payload", str(payload),
                "--settle-ms", "500",
                "--timeout-ms", "60000",
            ),
        )
        for payload in (64, 256, 1024)
        for workers in (1, 2, 4)
    ),
    Scenario(
        "core",
        "connection-churn-1000",
        "connection-churn",
        "gamenet.core_benchmark.v2",
        (
            "--scenario", "connection-churn",
            "--connections", "100",
            "--connect-concurrency", "16",
            "--iocp-accept-depth", "32",
            "--threads", "4",
            "--churn-rate", "1000",
            "--churn-duration-ms", "5000",
            "--churn-connect-timeout-ms", "1000",
            "--settle-ms", "0",
            "--timeout-ms", "60000",
        ),
        require_zero_churn_failures=True,
    ),
)

SCENARIO_PROFILES = {
    "regression": SCENARIOS,
    "core-capacity": CAPACITY_SCENARIOS,
}


class MatrixError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise MatrixError(message)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare_empty_directory(path: Path, label: str) -> None:
    if path.exists():
        require(path.is_dir(), f"{label} is not a directory: {path}")
        require(not any(path.iterdir()), f"{label} must be empty: {path}")
    else:
        path.mkdir(parents=True)


def validate_document(
    document: Any,
    scenario: Scenario,
    platform: str,
    backend: str,
    build_type: str,
    allow_legacy_core_v1: bool = True,
) -> dict[str, Any]:
    require(isinstance(document, dict), f"{scenario.key}: output must be a JSON object")
    allowed_schemas = {scenario.schema}
    if scenario.group == "core" and allow_legacy_core_v1:
        allowed_schemas.add("gamenet.core_benchmark.v1")
    require(document.get("schema") in allowed_schemas, f"{scenario.key}: schema mismatch")
    require(document.get("status") == "ok", f"{scenario.key}: benchmark status is not ok")
    require(document.get("error") is None, f"{scenario.key}: successful output must have null error")
    require(document.get("scenario") == scenario.reported_scenario, f"{scenario.key}: scenario mismatch")
    require(document.get("platform") == platform, f"{scenario.key}: platform mismatch")
    require(document.get("backend") == backend, f"{scenario.key}: backend mismatch")
    require(document.get("build_type") == build_type, f"{scenario.key}: build type mismatch")
    require(isinstance(document.get("parameters"), dict), f"{scenario.key}: parameters missing")
    require(isinstance(document.get("measurements"), dict), f"{scenario.key}: measurements missing")
    if document.get("schema") == "gamenet.core_benchmark.v2":
        try:
            validate_core_document(
                document,
                expected_platform=platform,
                expected_backend=backend,
                expected_build_type=build_type,
                expected_scenario=scenario.reported_scenario,
                require_zero_churn_failures=scenario.require_zero_churn_failures,
                label=scenario.key,
            )
        except CoreBenchmarkValidationError as error:
            raise MatrixError(str(error)) from error
    return document


def run_matrix(args: argparse.Namespace) -> dict[str, Any]:
    require(re.fullmatch(r"[0-9a-f]{40}", args.commit_sha) is not None, "commit SHA must be 40 lowercase hex digits")
    require(args.repetitions >= 3 and args.repetitions % 2 == 1, "repetitions must be an odd number of at least 3")
    scenarios = SCENARIO_PROFILES[args.matrix_profile]
    executable_arguments = {
        "core": args.core_executable,
        "phase4": args.phase4_executable,
    }
    executables = {}
    for group in {scenario.group for scenario in scenarios}:
        executable = executable_arguments[group]
        require(executable is not None, f"{group} benchmark executable argument missing")
        executables[group] = executable.resolve()
    for group, executable in executables.items():
        require(executable.is_file(), f"{group} benchmark executable missing: {executable}")
    prepare_empty_directory(args.output_root, "performance matrix output")
    for group in executables:
        (args.output_root / group).mkdir()

    manifest_scenarios = []
    for scenario in scenarios:
        samples = []
        parameters = None
        actual_schema = None
        for repetition in range(1, args.repetitions + 1):
            command = [str(executables[scenario.group]), *scenario.arguments]
            completed = subprocess.run(
                command,
                capture_output=True,
                timeout=args.process_timeout_seconds,
                check=False,
            )
            stderr = (
                completed.stderr
                if isinstance(completed.stderr, str)
                else completed.stderr.decode(
                    "utf-8",
                    errors="backslashreplace",
                )
            )
            require(
                completed.returncode == 0,
                f"{scenario.key} repetition {repetition} failed: {stderr.strip() or '<empty>'}",
            )
            try:
                stdout = (
                    completed.stdout
                    if isinstance(completed.stdout, str)
                    else completed.stdout.decode(
                        "utf-8",
                        errors="strict",
                    )
                )
            except UnicodeDecodeError as error:
                raise MatrixError(
                    f"{scenario.key} emitted non-UTF-8 stdout: {error}"
                ) from error
            try:
                document = json.loads(stdout.lstrip("\ufeff"))
            except json.JSONDecodeError as error:
                raise MatrixError(f"{scenario.key} emitted invalid JSON: {error}") from error
            validated = validate_document(
                document,
                scenario,
                args.platform,
                args.backend,
                args.build_type,
                allow_legacy_core_v1=args.matrix_profile == "regression",
            )
            if parameters is None:
                parameters = validated["parameters"]
            else:
                require(validated["parameters"] == parameters, f"{scenario.key}: parameters changed between repetitions")
            if actual_schema is None:
                actual_schema = validated["schema"]
            else:
                require(
                    validated["schema"] == actual_schema,
                    f"{scenario.key}: schema changed between repetitions",
                )
            relative = Path(scenario.group) / f"{scenario.key}-{repetition}.json"
            output = args.output_root / relative
            output.write_text(json.dumps(validated, indent=2) + "\n", encoding="utf-8")
            samples.append({"path": relative.as_posix(), "sha256": sha256_file(output)})

            if repetition == 1 and scenario.canonical:
                canonical_root = (
                    args.canonical_core_dir if scenario.group == "core" else args.canonical_phase4_dir
                )
                if canonical_root is not None:
                    canonical_root.mkdir(parents=True, exist_ok=True)
                    canonical = canonical_root / f"{scenario.key}.json"
                    require(not canonical.exists(), f"canonical benchmark output already exists: {canonical}")
                    shutil.copy2(output, canonical)
        manifest_scenarios.append({
            "key": f"{scenario.group}.{scenario.key}",
            "group": scenario.group,
            "reported_scenario": scenario.reported_scenario,
            "schema": actual_schema,
            "parameters": parameters,
            "samples": samples,
        })

    manifest = {
        "schema": SCHEMA,
        "profile": args.matrix_profile,
        "commit_sha": args.commit_sha,
        "platform": args.platform,
        "backend": args.backend,
        "build_type": args.build_type,
        "repetitions": args.repetitions,
        "executables": {
            group: {"path": str(path), "sha256": sha256_file(path)}
            for group, path in sorted(executables.items())
        },
        "scenarios": manifest_scenarios,
    }
    (args.output_root / "matrix-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the fixed GameNet performance regression matrix")
    parser.add_argument("--core-executable", type=Path, required=True)
    parser.add_argument("--phase4-executable", type=Path)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--canonical-core-dir", type=Path)
    parser.add_argument("--canonical-phase4-dir", type=Path)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument(
        "--matrix-profile",
        choices=tuple(SCENARIO_PROFILES),
        default="regression",
    )
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--process-timeout-seconds", type=int, default=90)
    args = parser.parse_args()
    try:
        manifest = run_matrix(args)
    except (MatrixError, OSError, subprocess.SubprocessError) as error:
        print(f"performance matrix failed: {error}", file=sys.stderr)
        return 1
    print(
        "performance matrix completed: "
        f"{manifest['profile']} "
        f"{len(manifest['scenarios'])} scenarios x {args.repetitions}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

