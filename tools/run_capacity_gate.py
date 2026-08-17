#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import validate_capacity_profile


SCHEMA = "gamenet.capacity_gate.v1"
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")


@dataclass(frozen=True)
class GateProfile:
    name: str
    repetitions: int
    parameters: dict[str, int | str]

    @property
    def endpoint_attempts(self) -> int:
        return (
            int(self.parameters["connections"])
            * int(self.parameters["messages"])
        )

    @property
    def probe_attempts(self) -> int:
        return (
            int(self.parameters["probe_target_per_second"])
            * int(self.parameters["probe_duration_ms"])
            // 1000
        )


def profile_parameters(
    *,
    connections: int,
    threads: int,
    messages: int,
    timeout_ms: int,
    probe_target_per_second: int,
    probe_duration_ms: int,
    probe_batch_size: int,
    probe_concurrency: int,
    reader_concurrency_limit: int,
) -> dict[str, int | str]:
    return {
        "scenario": "mixed-pressure-recovery",
        "connections": connections,
        "threads": threads,
        "messages": messages,
        "payload_bytes": 32768,
        "connection_low_water_bytes": 32768,
        "connection_high_water_bytes": 65536,
        "connection_hard_limit_bytes": 262144,
        "server_send_buffer_bytes": 4096,
        "recovery_pending_threshold_bytes": 0,
        "pressure_settle_ms": 500,
        "recovery_stable_ms": 250,
        "timeout_ms": timeout_ms,
        "iocp_accept_depth": 32,
        "probe_target_per_second": probe_target_per_second,
        "probe_duration_ms": probe_duration_ms,
        "probe_batch_size": probe_batch_size,
        "probe_concurrency": probe_concurrency,
        "probe_payload_bytes": 32,
        "probe_connect_timeout_ms": min(2000, timeout_ms),
        "reader_concurrency_limit": reader_concurrency_limit,
    }


PROFILES = {
    "candidate-10k": GateProfile(
        name="candidate-10k",
        repetitions=3,
        parameters=profile_parameters(
            connections=1000,
            threads=4,
            messages=10,
            timeout_ms=120000,
            probe_target_per_second=100,
            probe_duration_ms=5000,
            probe_batch_size=10,
            probe_concurrency=4,
            reader_concurrency_limit=16,
        ),
    ),
    "dedicated-100k": GateProfile(
        name="dedicated-100k",
        repetitions=1,
        parameters=profile_parameters(
            connections=10000,
            threads=8,
            messages=10,
            timeout_ms=300000,
            probe_target_per_second=1000,
            probe_duration_ms=10000,
            probe_batch_size=100,
            probe_concurrency=16,
            reader_concurrency_limit=64,
        ),
    ),
}

PARAMETER_ARGUMENTS = (
    ("scenario", "--scenario"),
    ("connections", "--connections"),
    ("threads", "--threads"),
    ("messages", "--messages"),
    ("payload_bytes", "--payload-bytes"),
    ("connection_low_water_bytes", "--low-water-bytes"),
    ("connection_high_water_bytes", "--high-water-bytes"),
    ("connection_hard_limit_bytes", "--hard-limit-bytes"),
    ("server_send_buffer_bytes", "--server-send-buffer-bytes"),
    (
        "recovery_pending_threshold_bytes",
        "--recovery-threshold-bytes",
    ),
    ("pressure_settle_ms", "--pressure-settle-ms"),
    ("recovery_stable_ms", "--recovery-stable-ms"),
    ("timeout_ms", "--timeout-ms"),
    ("iocp_accept_depth", "--iocp-accept-depth"),
    ("probe_target_per_second", "--probe-rate"),
    ("probe_duration_ms", "--probe-duration-ms"),
    ("probe_batch_size", "--probe-batch-size"),
    ("probe_concurrency", "--probe-concurrency"),
    ("probe_payload_bytes", "--probe-payload-bytes"),
    ("probe_connect_timeout_ms", "--probe-connect-timeout-ms"),
    ("reader_concurrency_limit", "--reader-concurrency"),
)


class CapacityGateError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CapacityGateError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_for(executable: Path, profile: GateProfile) -> list[str]:
    command = [str(executable)]
    for name, option in PARAMETER_ARGUMENTS:
        command.extend((option, str(profile.parameters[name])))
    return command


def retain_failed_sample(
    output_root: Path,
    repetition: int,
    stdout: str,
) -> str:
    if not stdout.strip():
        return "sample emitted no stdout"
    path = output_root / f"sample-{repetition}-failure.json"
    path.write_text(stdout, encoding="utf-8")
    details = [f"stdout retained as {path.name}"]
    try:
        document = json.loads(stdout.lstrip("\ufeff"))
    except json.JSONDecodeError as error:
        details.append(f"stdout JSON parse failed: {error}")
        return "; ".join(details)
    if isinstance(document, dict):
        reported_error = document.get("error")
        if isinstance(reported_error, str) and reported_error:
            details.append(f"reported error: {reported_error}")
        checks = document.get("checks")
        if isinstance(checks, dict):
            failed_checks = sorted(
                name
                for name, value in checks.items()
                if value is False
            )
            if failed_checks:
                details.append(
                    "failed checks: " + ", ".join(failed_checks)
                )
    return "; ".join(details)


def validate_gate_document(
    document: Any,
    profile: GateProfile,
    *,
    platform: str,
    backend: str,
    build_type: str,
    label: str,
) -> dict[str, Any]:
    validated = validate_capacity_profile.validate_document(
        document,
        expected_platform=platform,
        expected_backend=backend,
        expected_build_type=build_type,
        expected_connections=int(profile.parameters["connections"]),
        label=label,
    )
    require(
        validated.get("schema") == validate_capacity_profile.SCHEMA_V3,
        f"{label}: capacity gate requires the scale-ready v3 schema",
    )
    parameters = validated.get("parameters")
    limits = validated.get("limits")
    require(isinstance(parameters, dict), f"{label}: parameters are missing")
    require(isinstance(limits, dict), f"{label}: limits are missing")
    for name in (
        "connections",
        "threads",
        "messages",
        "payload_bytes",
        "server_send_buffer_bytes",
        "pressure_settle_ms",
        "recovery_stable_ms",
        "timeout_ms",
        "iocp_accept_depth",
        "probe_target_per_second",
        "probe_duration_ms",
        "probe_batch_size",
        "probe_concurrency",
        "probe_payload_bytes",
        "probe_connect_timeout_ms",
        "reader_concurrency_limit",
    ):
        require(
            parameters.get(name) == profile.parameters[name],
            f"{label}: gate parameter mismatch: {name}",
        )
    for name in (
        "connection_low_water_bytes",
        "connection_high_water_bytes",
        "connection_hard_limit_bytes",
        "recovery_pending_threshold_bytes",
    ):
        require(
            limits.get(name) == profile.parameters[name],
            f"{label}: gate limit mismatch: {name}",
        )
    terminal = validated.get("terminal")
    healthy = validated.get("healthy_churn")
    require(isinstance(terminal, dict), f"{label}: terminal result is missing")
    require(isinstance(healthy, dict), f"{label}: healthy churn is missing")
    require(
        terminal.get("scheduled_endpoints") == profile.endpoint_attempts,
        f"{label}: endpoint-attempt scale mismatch",
    )
    require(
        healthy.get("attempted") == profile.probe_attempts,
        f"{label}: probe-attempt scale mismatch",
    )
    return validated


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a fixed GameNet mixed capacity evidence profile"
    )
    parser.add_argument("--profile", choices=sorted(PROFILES), required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", type=int, required=True)
    parser.add_argument("--job", required=True)
    parser.add_argument("--artifact-name", required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    return parser.parse_args(argv)


def run_gate(arguments: argparse.Namespace) -> dict[str, Any]:
    profile = PROFILES[arguments.profile]
    executable = arguments.executable.resolve()
    toolchain = arguments.toolchain.resolve()
    require(executable.is_file(), f"capacity executable is missing: {executable}")
    require(toolchain.is_file(), f"toolchain evidence is missing: {toolchain}")
    require(
        SHA_PATTERN.fullmatch(arguments.candidate_sha) is not None,
        "candidate SHA must be a full lowercase Git SHA",
    )
    require(arguments.run_attempt > 0, "run attempt must be positive")
    require(
        (arguments.platform, arguments.backend)
        in {("linux", "epoll"), ("windows", "iocp")},
        "platform/backend pair is invalid",
    )
    require(arguments.build_type == "Release", "capacity gate must use Release")
    expected_artifact_name = (
        f"capacity-gate-{profile.name}-{arguments.job}-"
        f"{arguments.candidate_sha}-{arguments.run_id}-"
        f"{arguments.run_attempt}"
    )
    require(
        arguments.artifact_name == expected_artifact_name,
        "capacity artifact name is not canonical",
    )

    output_root = arguments.output_root.resolve()
    require(
        not output_root.exists(),
        f"capacity output root already exists: {output_root}",
    )
    output_root.mkdir(parents=True)
    copied_toolchain = output_root / "toolchain.txt"
    copied_toolchain.write_bytes(toolchain.read_bytes())

    samples: list[dict[str, Any]] = []
    command = command_for(executable, profile)
    process_timeout = int(profile.parameters["timeout_ms"]) / 1000.0 + 120.0
    for repetition in range(1, profile.repetitions + 1):
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=process_timeout,
            check=False,
        )
        if completed.returncode != 0:
            retained_failure = retain_failed_sample(
                output_root,
                repetition,
                completed.stdout,
            )
            stderr = completed.stderr.strip() or "<empty>"
            raise CapacityGateError(
                f"capacity sample {repetition} failed with "
                f"exit {completed.returncode}; stderr: {stderr}; "
                f"{retained_failure}"
            )
        try:
            document = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise CapacityGateError(
                f"capacity sample {repetition} emitted invalid JSON: {error}"
            ) from error
        validate_gate_document(
            document,
            profile,
            platform=arguments.platform,
            backend=arguments.backend,
            build_type=arguments.build_type,
            label=f"{profile.name} sample {repetition}",
        )
        relative = f"sample-{repetition}.json"
        path = output_root / relative
        path.write_text(completed.stdout, encoding="utf-8")
        samples.append(
            {
                "repetition": repetition,
                "path": relative,
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )

    manifest = {
        "schema": SCHEMA,
        "result": "pass",
        "profile": profile.name,
        "candidate_sha": arguments.candidate_sha,
        "run_id": str(arguments.run_id),
        "run_attempt": arguments.run_attempt,
        "job": arguments.job,
        "artifact_name": arguments.artifact_name,
        "platform": arguments.platform,
        "backend": arguments.backend,
        "build_type": arguments.build_type,
        "repetitions": profile.repetitions,
        "endpoint_attempts": profile.endpoint_attempts,
        "probe_attempts": profile.probe_attempts,
        "parameters": profile.parameters,
        "executable_sha256": sha256_file(executable),
        "toolchain": {
            "path": copied_toolchain.name,
            "bytes": copied_toolchain.stat().st_size,
            "sha256": sha256_file(copied_toolchain),
        },
        "samples": samples,
    }
    (output_root / "capacity-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest


def main(argv: list[str] | None = None) -> int:
    try:
        arguments = parse_args(sys.argv[1:] if argv is None else argv)
        manifest = run_gate(arguments)
    except (
        OSError,
        subprocess.SubprocessError,
        CapacityGateError,
        validate_capacity_profile.CapacityProfileValidationError,
    ) as error:
        print(f"capacity gate failed: {error}", file=sys.stderr)
        return 1
    print(
        f"capacity gate passed: {manifest['profile']} "
        f"{manifest['platform']}/{manifest['backend']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
