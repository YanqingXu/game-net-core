from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


SCHEMA = "gamenet.ci_evidence.v1"
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
VALID_STATUSES = {"success", "failure", "cancelled", "skipped"}


class EvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def environment_value(name: str, *, required: bool = True) -> str | None:
    value = os.environ.get(name, "").strip()
    if not value:
        if required:
            raise EvidenceError(f"missing environment variable: {name}")
        return None
    require("\n" not in value and "\r" not in value, f"multiline environment value: {name}")
    return value


def validate_sha(name: str, value: str | None, *, required: bool = True) -> str | None:
    if value is None:
        require(not required, f"missing SHA: {name}")
        return None
    require(SHA_PATTERN.fullmatch(value) is not None, f"invalid SHA for {name}: {value!r}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checkout_sha() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "HEAD"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    require(result.returncode == 0, f"cannot resolve checkout HEAD: {result.stderr.strip()}")
    value = validate_sha("checkout HEAD", result.stdout.strip())
    require(value is not None, "checkout HEAD is missing")
    return value


def collect_evidence(evidence_root: Path, output: Path) -> list[dict[str, object]]:
    root = evidence_root.resolve()
    output_path = output.resolve()
    try:
        output_path.relative_to(root)
    except ValueError as error:
        raise EvidenceError("manifest output must stay inside the evidence root") from error

    files: list[dict[str, object]] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        require(not path.is_symlink(), f"evidence must not contain symlinks: {path}")
        if not path.is_file() or path.resolve() == output_path:
            continue
        relative_path = path.relative_to(root).as_posix()
        files.append(
            {
                "path": relative_path,
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return files


def build_manifest(
    evidence_root: Path,
    output: Path,
    artifact_name: str,
    commands: list[str],
    require_canonical_artifact_name: bool = False,
) -> dict[str, object]:
    require(bool(artifact_name.strip()), "artifact name must not be empty")
    require("\n" not in artifact_name and "\r" not in artifact_name, "artifact name must be one line")
    require(bool(commands), "at least one command is required")
    for command in commands:
        require(bool(command.strip()), "commands must not be empty")

    checkout = checkout_sha()
    github_sha = validate_sha("GITHUB_SHA", environment_value("GITHUB_SHA"))
    pr_head_sha = validate_sha(
        "GAMENET_CI_PR_HEAD_SHA",
        environment_value("GAMENET_CI_PR_HEAD_SHA", required=False),
        required=False,
    )
    candidate_sha = validate_sha(
        "GAMENET_CI_CANDIDATE_SHA",
        environment_value("GAMENET_CI_CANDIDATE_SHA", required=False)
        or pr_head_sha
        or github_sha,
    )
    merge_or_current_sha = validate_sha(
        "GAMENET_CI_MERGE_OR_CURRENT_SHA",
        environment_value("GAMENET_CI_MERGE_OR_CURRENT_SHA", required=False) or github_sha,
    )

    require(checkout == github_sha, "checkout SHA must equal GITHUB_SHA")
    require(
        merge_or_current_sha == github_sha,
        "merge/current SHA must equal GITHUB_SHA",
    )

    run_id = environment_value("GITHUB_RUN_ID")
    run_attempt = environment_value("GITHUB_RUN_ATTEMPT")
    require(run_id is not None and run_id.isdecimal(), f"invalid GITHUB_RUN_ID: {run_id!r}")
    require(
        run_attempt is not None and run_attempt.isdecimal() and int(run_attempt, 10) >= 1,
        f"invalid GITHUB_RUN_ATTEMPT: {run_attempt!r}",
    )
    status = environment_value("GAMENET_CI_STATUS")
    require(status in VALID_STATUSES, f"invalid CI status: {status!r}")
    job = environment_value("GITHUB_JOB")
    workflow = environment_value("GITHUB_WORKFLOW")
    event_name = environment_value("GITHUB_EVENT_NAME")

    if event_name == "pull_request":
        require(pr_head_sha is not None, "pull_request evidence requires PR head SHA")
        require(candidate_sha == pr_head_sha, "candidate SHA must equal PR head SHA")
    else:
        require(candidate_sha == github_sha, "non-PR candidate SHA must equal GITHUB_SHA")

    if require_canonical_artifact_name:
        expected_suffix = f"-{job}-{github_sha}-{run_id}-{int(run_attempt, 10)}"
        require(
            artifact_name.endswith(expected_suffix),
            "artifact name must include job, GITHUB_SHA, run id, and run attempt",
        )

    generated_at = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    return {
        "schema": SCHEMA,
        "generated_at_utc": generated_at,
        "date_utc": generated_at[:10],
        "checkout_sha": checkout,
        "status": status,
        "pre_upload_status": status,
        "status_scope": "job_before_evidence_upload",
        "candidate_sha": candidate_sha,
        "github_sha": github_sha,
        "pr_head_sha": pr_head_sha,
        "merge_or_current_sha": merge_or_current_sha,
        "run_id": run_id,
        "run_attempt": int(run_attempt, 10),
        "job": job,
        "workflow": workflow,
        "ref": environment_value("GITHUB_REF"),
        "event_name": event_name,
        "repository": environment_value("GITHUB_REPOSITORY"),
        "runner": {
            "os": environment_value("RUNNER_OS"),
            "arch": environment_value("RUNNER_ARCH"),
        },
        "artifact_name": artifact_name,
        "commands": commands,
        "evidence_files": collect_evidence(evidence_root, output),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Write a SHA-bound manifest for the evidence produced by one CI job."
    )
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--artifact-name", required=True)
    parser.add_argument("--command", action="append", required=True)
    parser.add_argument("--require-canonical-artifact-name", action="store_true")
    arguments = parser.parse_args()

    try:
        arguments.evidence_root.mkdir(parents=True, exist_ok=True)
        manifest = build_manifest(
            arguments.evidence_root,
            arguments.output,
            arguments.artifact_name,
            arguments.command,
            arguments.require_canonical_artifact_name,
        )
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except (EvidenceError, OSError) as error:
        print(f"CI evidence manifest failed: {error}", file=sys.stderr)
        return 1

    print(
        f"wrote {arguments.output} with {len(manifest['evidence_files'])} hashed evidence files"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
