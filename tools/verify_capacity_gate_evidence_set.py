#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

import run_capacity_gate


SCHEMA = "gamenet.capacity_gate_pair.v1"
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
EXPECTED_JOBS = {
    "linux-capacity-gate": ("linux", "epoll"),
    "windows-capacity-gate": ("windows", "iocp"),
}


class CapacityGatePairError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CapacityGatePairError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CapacityGatePairError(
            f"cannot read {label} {path}: {error}"
        ) from error
    require(isinstance(document, dict), f"{label} must be a JSON object")
    return document


def checked_relative_file(
    root: Path,
    relative: Any,
    *,
    label: str,
) -> Path:
    require(isinstance(relative, str) and relative, f"{label} path is missing")
    path = (root / relative).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError as error:
        raise CapacityGatePairError(f"{label} path escapes its artifact") from error
    require(path.is_file() and not path.is_symlink(), f"{label} file is missing")
    return path


def verify_hashed_file(
    root: Path,
    entry: Any,
    *,
    label: str,
) -> tuple[Path, str]:
    require(isinstance(entry, dict), f"{label} entry must be an object")
    path = checked_relative_file(root, entry.get("path"), label=label)
    require(path.stat().st_size == entry.get("bytes"), f"{label} byte count mismatch")
    digest = sha256_file(path)
    require(digest == entry.get("sha256"), f"{label} hash mismatch")
    return path, digest


def verify_manifest(
    path: Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    manifest = load_json(path, "capacity manifest")
    root = path.parent
    require(
        manifest.get("schema") == run_capacity_gate.SCHEMA,
        "unexpected capacity manifest schema",
    )
    require(manifest.get("result") == "pass", "capacity producer did not pass")
    profile_name = manifest.get("profile")
    require(
        profile_name in run_capacity_gate.PROFILES,
        "capacity profile is not in the reviewed inventory",
    )
    profile = run_capacity_gate.PROFILES[str(profile_name)]
    job = manifest.get("job")
    require(job in EXPECTED_JOBS, f"unexpected capacity job: {job!r}")
    platform, backend = EXPECTED_JOBS[str(job)]
    require(manifest.get("platform") == platform, "capacity platform mismatch")
    require(manifest.get("backend") == backend, "capacity backend mismatch")
    require(manifest.get("build_type") == "Release", "capacity build type mismatch")
    require(
        manifest.get("repetitions") == profile.repetitions,
        "capacity repetition contract drifted",
    )
    require(
        manifest.get("endpoint_attempts") == profile.endpoint_attempts,
        "capacity endpoint-attempt scale drifted",
    )
    require(
        manifest.get("probe_attempts") == profile.probe_attempts,
        "capacity probe-attempt scale drifted",
    )
    require(
        manifest.get("parameters") == profile.parameters,
        "capacity parameters drifted from the reviewed profile",
    )
    candidate_sha = manifest.get("candidate_sha")
    require(
        isinstance(candidate_sha, str)
        and run_capacity_gate.SHA_PATTERN.fullmatch(candidate_sha) is not None,
        "capacity candidate SHA is invalid",
    )
    run_id = manifest.get("run_id")
    run_attempt = manifest.get("run_attempt")
    require(isinstance(run_id, str) and run_id, "capacity run ID is missing")
    require(
        isinstance(run_attempt, int)
        and not isinstance(run_attempt, bool)
        and run_attempt > 0,
        "capacity run attempt is invalid",
    )
    artifact_name = manifest.get("artifact_name")
    expected_name = (
        f"capacity-gate-{profile.name}-{job}-{candidate_sha}-"
        f"{run_id}-{run_attempt}"
    )
    require(artifact_name == expected_name, "capacity artifact name is not canonical")
    require(root.name == artifact_name, "capacity artifact directory name mismatch")
    executable_sha = manifest.get("executable_sha256")
    require(
        isinstance(executable_sha, str)
        and SHA256_PATTERN.fullmatch(executable_sha) is not None,
        "capacity executable hash is invalid",
    )

    verify_hashed_file(root, manifest.get("toolchain"), label="capacity toolchain")
    samples = manifest.get("samples")
    require(
        isinstance(samples, list)
        and len(samples) == profile.repetitions,
        "capacity sample inventory mismatch",
    )
    verified_samples: list[dict[str, Any]] = []
    seen_paths: set[str] = set()
    for expected_repetition, entry in enumerate(samples, start=1):
        require(isinstance(entry, dict), "capacity sample entry must be an object")
        require(
            entry.get("repetition") == expected_repetition,
            "capacity repetition ordering mismatch",
        )
        relative = entry.get("path")
        require(relative not in seen_paths, "duplicate capacity sample path")
        seen_paths.add(str(relative))
        sample_path, digest = verify_hashed_file(
            root,
            entry,
            label=f"capacity sample {expected_repetition}",
        )
        document = load_json(sample_path, "capacity sample")
        try:
            run_capacity_gate.validate_gate_document(
                document,
                profile,
                platform=platform,
                backend=backend,
                build_type="Release",
                label=f"{job} sample {expected_repetition}",
            )
        except (
            run_capacity_gate.CapacityGateError,
            run_capacity_gate.validate_capacity_profile.CapacityProfileValidationError,
        ) as error:
            raise CapacityGatePairError(str(error)) from error
        verified_samples.append(
            {
                "repetition": expected_repetition,
                "path": str(relative),
                "sha256": digest,
            }
        )

    return manifest, {
        "job": job,
        "platform": platform,
        "backend": backend,
        "artifact_name": artifact_name,
        "manifest_sha256": sha256_file(path),
        "executable_sha256": executable_sha,
        "samples": verified_samples,
    }


def verify_evidence_set(input_root: Path) -> dict[str, Any]:
    require(input_root.is_dir(), f"capacity evidence root is missing: {input_root}")
    manifests = sorted(input_root.rglob("capacity-manifest.json"))
    require(
        len(manifests) == len(EXPECTED_JOBS),
        f"expected {len(EXPECTED_JOBS)} capacity manifests, got {len(manifests)}",
    )
    verified: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {}
    for path in manifests:
        manifest, summary = verify_manifest(path)
        job = str(manifest["job"])
        require(job not in verified, f"duplicate capacity producer: {job}")
        verified[job] = (manifest, summary)
    require(set(verified) == set(EXPECTED_JOBS), "capacity producer set mismatch")

    identity_fields = (
        "profile",
        "candidate_sha",
        "run_id",
        "run_attempt",
        "repetitions",
        "endpoint_attempts",
        "probe_attempts",
        "parameters",
    )
    first_manifest = next(iter(verified.values()))[0]
    for manifest, _ in verified.values():
        for field in identity_fields:
            require(
                manifest.get(field) == first_manifest.get(field),
                f"Linux/Windows capacity identity mismatch: {field}",
            )

    return {
        "schema": SCHEMA,
        "result": "pass",
        "profile": first_manifest["profile"],
        "candidate_sha": first_manifest["candidate_sha"],
        "run_id": first_manifest["run_id"],
        "run_attempt": first_manifest["run_attempt"],
        "repetitions": first_manifest["repetitions"],
        "endpoint_attempts": first_manifest["endpoint_attempts"],
        "probe_attempts": first_manifest["probe_attempts"],
        "parameters": first_manifest["parameters"],
        "platforms": [
            summary
            for _, summary in (
                verified[job]
                for job in sorted(verified)
            )
        ],
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify paired Linux/Windows mixed capacity evidence"
    )
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        result = verify_evidence_set(arguments.input_root.resolve())
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except (OSError, CapacityGatePairError) as error:
        print(f"capacity evidence verification failed: {error}", file=sys.stderr)
        return 1
    print(
        f"paired capacity evidence passed: {result['profile']} "
        f"{result['candidate_sha']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
