from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.phase4_benchmark_pair_evidence.v1"
JOB_SCHEMA = "gamenet.ci_evidence.v1"
BENCHMARK_SCHEMA = "gamenet.phase4_benchmark_evidence.v1"
EXPECTED_JOBS = {
    "linux-release-benchmark": ("linux", "epoll"),
    "windows-release-benchmark": ("windows", "iocp"),
}
EXPECTED_SCENARIOS = ("framing", "logic-queue", "broadcast-fanout")
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")


class PairEvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PairEvidenceError(message)


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PairEvidenceError(f"cannot read {label} {path}: {error}") from error
    require(isinstance(document, dict), f"{label} must be a JSON object: {path}")
    return document


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_job_files(job_root: Path, manifest: dict[str, Any]) -> set[str]:
    entries = manifest.get("evidence_files")
    require(isinstance(entries, list) and entries, "job manifest has no evidence files")
    seen: set[str] = set()
    for entry in entries:
        require(isinstance(entry, dict), "job manifest evidence entry must be an object")
        relative = entry.get("path")
        require(isinstance(relative, str) and relative, "job manifest evidence path is missing")
        require(relative not in seen, f"duplicate job evidence path: {relative}")
        seen.add(relative)
        path = (job_root / relative).resolve()
        try:
            path.relative_to(job_root.resolve())
        except ValueError as error:
            raise PairEvidenceError(f"job evidence path escapes artifact root: {relative}") from error
        require(path.is_file() and not path.is_symlink(), f"job evidence file is missing: {relative}")
        require(path.stat().st_size == entry.get("bytes"), f"job evidence byte count mismatch: {relative}")
        require(sha256_file(path) == entry.get("sha256"), f"job evidence hash mismatch: {relative}")
    return seen


def identity_tuple(manifest: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(
        manifest.get(key)
        for key in (
            "checkout_sha",
            "candidate_sha",
            "github_sha",
            "pr_head_sha",
            "merge_or_current_sha",
            "run_id",
            "run_attempt",
            "workflow",
            "ref",
            "event_name",
            "repository",
        )
    )


def verify_set(input_root: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    require(input_root.is_dir(), f"benchmark evidence root does not exist: {input_root}")
    manifest_paths = sorted(input_root.rglob("job-manifest.json"))
    require(
        len(manifest_paths) == len(EXPECTED_JOBS),
        f"expected {len(EXPECTED_JOBS)} benchmark job manifests, got {len(manifest_paths)}",
    )

    by_job: dict[str, tuple[Path, dict[str, Any]]] = {}
    for manifest_path in manifest_paths:
        manifest = load_json(manifest_path, "benchmark job manifest")
        require(manifest.get("schema") == JOB_SCHEMA, f"unexpected job manifest schema: {manifest_path}")
        job = manifest.get("job")
        require(job in EXPECTED_JOBS, f"unexpected benchmark job: {job!r}")
        require(job not in by_job, f"duplicate benchmark job manifest: {job}")
        require(manifest.get("status") == "success", f"benchmark job did not succeed: {job}")
        require(manifest.get("pre_upload_status") == "success", f"benchmark job pre-upload status is not success: {job}")
        checkout = manifest.get("checkout_sha")
        require(isinstance(checkout, str) and SHA_PATTERN.fullmatch(checkout), f"invalid checkout SHA: {job}")
        evidence_paths = verify_job_files(manifest_path.parent, manifest)
        require("toolchain.txt" in evidence_paths, f"benchmark toolchain evidence is missing: {job}")
        require("evidence-manifest.json" in evidence_paths, f"semantic manifest is not hashed: {job}")
        commands = manifest.get("commands")
        require(isinstance(commands, list) and commands, f"benchmark commands are missing: {job}")
        by_job[job] = (manifest_path.parent, manifest)

    require(set(by_job) == set(EXPECTED_JOBS), f"benchmark job set drift: {sorted(by_job)}")
    identities = {identity_tuple(manifest) for _, manifest in by_job.values()}
    require(len(identities) == 1, "Linux and Windows benchmark job identities do not match")

    common = next(iter(by_job.values()))[1]
    platforms: list[dict[str, Any]] = []
    common_parameters: dict[str, Any] | None = None
    for job, (job_root, job_manifest) in sorted(by_job.items()):
        expected_platform, expected_backend = EXPECTED_JOBS[job]
        runner = job_manifest.get("runner")
        require(isinstance(runner, dict), f"runner identity is missing: {job}")
        require(
            str(runner.get("os", "")).lower() == expected_platform,
            f"runner OS does not match benchmark platform: {job}",
        )
        semantic_path = job_root / "evidence-manifest.json"
        semantic = load_json(semantic_path, "benchmark semantic manifest")
        require(semantic.get("schema") == BENCHMARK_SCHEMA, f"unexpected semantic schema: {job}")
        require(semantic.get("commit_sha") == job_manifest.get("checkout_sha"), f"semantic commit mismatch: {job}")
        require(str(semantic.get("run_id")) == str(job_manifest.get("run_id")), f"semantic run mismatch: {job}")
        require(semantic.get("job") == job, f"semantic job mismatch: {job}")
        require(
            semantic.get("artifact_name") == job_manifest.get("artifact_name"),
            f"semantic artifact name mismatch: {job}",
        )
        require(semantic.get("platform") == expected_platform, f"platform mismatch: {job}")
        require(semantic.get("backend") == expected_backend, f"backend mismatch: {job}")
        require(semantic.get("build_type") == "Release", f"build type mismatch: {job}")

        artifacts = semantic.get("artifacts")
        require(isinstance(artifacts, list), f"semantic artifacts must be an array: {job}")
        require(
            [artifact.get("scenario") for artifact in artifacts] == list(EXPECTED_SCENARIOS),
            f"scenario set/order mismatch: {job}",
        )
        parameters: dict[str, Any] = {}
        for artifact in artifacts:
            scenario = artifact["scenario"]
            raw_path = job_root / str(artifact.get("file"))
            require(raw_path.is_file(), f"missing raw benchmark file: {job}: {scenario}")
            require(sha256_file(raw_path) == artifact.get("sha256"), f"raw benchmark hash mismatch: {job}: {scenario}")
            parameters[scenario] = artifact.get("parameters")
        if common_parameters is None:
            common_parameters = parameters
        else:
            require(parameters == common_parameters, "Linux and Windows benchmark parameters do not match")

        platforms.append(
            {
                "job": job,
                "platform": expected_platform,
                "backend": expected_backend,
                "artifact_name": job_manifest["artifact_name"],
                "job_manifest_sha256": sha256_file(job_root / "job-manifest.json"),
                "semantic_manifest_sha256": sha256_file(semantic_path),
            }
        )

    generated_at = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    aggregate = {
        "schema": SCHEMA,
        "generated_at_utc": generated_at,
        "result": "success",
        "checkout_sha": common["checkout_sha"],
        "candidate_sha": common["candidate_sha"],
        "github_sha": common["github_sha"],
        "pr_head_sha": common.get("pr_head_sha"),
        "merge_or_current_sha": common["merge_or_current_sha"],
        "run_id": common["run_id"],
        "run_attempt": common["run_attempt"],
        "workflow": common["workflow"],
        "repository": common["repository"],
        "parameters": common_parameters,
        "platforms": platforms,
    }
    return aggregate, platforms


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify paired Linux/Windows Phase 4 benchmark evidence from one workflow identity."
    )
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        aggregate, platforms = verify_set(arguments.input_root)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(aggregate, indent=2) + "\n", encoding="utf-8")
    except (PairEvidenceError, OSError) as error:
        print(f"Phase 4 benchmark evidence-set verification failed: {error}", file=sys.stderr)
        return 1
    print(f"verified paired Phase 4 benchmark evidence for {len(platforms)} platforms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
