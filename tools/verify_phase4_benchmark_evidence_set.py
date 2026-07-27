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
PERFORMANCE_SCHEMA = "gamenet.performance_regression.v1"
MATRIX_SCHEMA = "gamenet.performance_matrix.v1"
PERFORMANCE_BASELINE_SHA = "2b1be4343f7c478eb40542451f30aad8ca474003"
PERFORMANCE_REPETITIONS = 3
EXPECTED_JOBS = {
    "linux-release-benchmark": ("linux", "epoll"),
    "windows-release-benchmark": ("windows", "iocp"),
}
EXPECTED_SCENARIOS = ("framing", "logic-queue", "broadcast-fanout")
EXPECTED_PERFORMANCE_SCENARIOS = {
    "core.connections-256",
    "core.connections-1024",
    "core.echo-1-worker",
    "core.echo-2-workers",
    "core.echo-4-workers",
    "core.slow-client-4",
    "core.slow-client-16",
    "phase4.broadcast-fanout",
    "phase4.broadcast-fanout-4096",
    "phase4.framing",
    "phase4.logic-queue",
    "phase4.logic-queue-heavy",
}
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


def verify_matrix_manifest(
    job_root: Path,
    relative_root: str,
    expected_sha: str,
    platform: str,
    backend: str,
    evidence_paths: set[str],
) -> str:
    manifest_relative = f"{relative_root}/matrix-manifest.json"
    require(manifest_relative in evidence_paths, f"performance matrix manifest is not hashed: {manifest_relative}")
    matrix_root = job_root / relative_root
    manifest_path = matrix_root / "matrix-manifest.json"
    matrix = load_json(manifest_path, "performance matrix manifest")
    require(matrix.get("schema") == MATRIX_SCHEMA, "unexpected performance matrix schema")
    require(matrix.get("commit_sha") == expected_sha, "performance matrix commit mismatch")
    require(matrix.get("platform") == platform, "performance matrix platform mismatch")
    require(matrix.get("backend") == backend, "performance matrix backend mismatch")
    require(matrix.get("build_type") == "Release", "performance matrix build type mismatch")
    require(matrix.get("repetitions") == PERFORMANCE_REPETITIONS, "performance matrix repetition mismatch")
    scenarios = matrix.get("scenarios")
    require(isinstance(scenarios, list), "performance matrix scenarios must be an array")
    require(
        {item.get("key") for item in scenarios if isinstance(item, dict)} == EXPECTED_PERFORMANCE_SCENARIOS,
        "performance matrix scenario inventory mismatch",
    )
    for scenario in scenarios:
        require(isinstance(scenario, dict), "performance matrix scenario must be an object")
        samples = scenario.get("samples")
        require(
            isinstance(samples, list) and len(samples) == PERFORMANCE_REPETITIONS,
            f"performance matrix sample count mismatch: {scenario.get('key')}",
        )
        for sample in samples:
            require(isinstance(sample, dict), "performance matrix sample must be an object")
            relative = sample.get("path")
            require(isinstance(relative, str) and relative, "performance matrix sample path missing")
            job_relative = f"{relative_root}/{relative}"
            require(job_relative in evidence_paths, f"performance sample is not hashed: {job_relative}")
            sample_path = matrix_root / relative
            require(sample_path.is_file(), f"performance sample missing: {job_relative}")
            require(sha256_file(sample_path) == sample.get("sha256"), f"performance sample hash mismatch: {job_relative}")
    return sha256_file(manifest_path)


def verify_set(input_root: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    require(input_root.is_dir(), f"benchmark evidence root does not exist: {input_root}")
    manifest_paths = sorted(input_root.rglob("job-manifest.json"))
    require(
        len(manifest_paths) == len(EXPECTED_JOBS),
        f"expected {len(EXPECTED_JOBS)} benchmark job manifests, got {len(manifest_paths)}",
    )

    by_job: dict[str, tuple[Path, dict[str, Any], set[str]]] = {}
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
        by_job[job] = (manifest_path.parent, manifest, evidence_paths)

    require(set(by_job) == set(EXPECTED_JOBS), f"benchmark job set drift: {sorted(by_job)}")
    identities = {identity_tuple(manifest) for _, manifest, _ in by_job.values()}
    require(len(identities) == 1, "Linux and Windows benchmark job identities do not match")

    common = next(iter(by_job.values()))[1]
    platforms: list[dict[str, Any]] = []
    common_parameters: dict[str, Any] | None = None
    common_performance_contract: list[tuple[str, tuple[tuple[Any, ...], ...]]] | None = None
    for job, (job_root, job_manifest, evidence_paths) in sorted(by_job.items()):
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

        performance_relative = "performance-regression.json"
        require(performance_relative in evidence_paths, f"performance regression evidence is not hashed: {job}")
        performance_path = job_root / performance_relative
        performance = load_json(performance_path, "performance regression evidence")
        require(performance.get("schema") == PERFORMANCE_SCHEMA, f"unexpected performance schema: {job}")
        require(performance.get("result") == "pass", f"performance regression gate did not pass: {job}")
        require(performance.get("platform") == expected_platform, f"performance platform mismatch: {job}")
        require(performance.get("backend") == expected_backend, f"performance backend mismatch: {job}")
        require(performance.get("baseline_sha") == PERFORMANCE_BASELINE_SHA, f"performance baseline mismatch: {job}")
        require(performance.get("candidate_sha") == job_manifest.get("checkout_sha"), f"performance candidate mismatch: {job}")
        require(performance.get("repetitions") == PERFORMANCE_REPETITIONS, f"performance repetitions mismatch: {job}")
        budget_sha = performance.get("budget_sha256")
        require(isinstance(budget_sha, str) and re.fullmatch(r"[0-9a-f]{64}", budget_sha) is not None,
                f"invalid performance budget hash: {job}")
        comparisons = performance.get("comparisons")
        require(isinstance(comparisons, list), f"performance comparisons missing: {job}")
        require({item.get("key") for item in comparisons if isinstance(item, dict)} == EXPECTED_PERFORMANCE_SCENARIOS,
                f"performance comparison inventory mismatch: {job}")
        contract = []
        for comparison in comparisons:
            require(isinstance(comparison, dict), f"performance comparison must be an object: {job}")
            require(comparison.get("result") == "pass", f"performance scenario did not pass: {job}")
            metrics = comparison.get("metrics")
            require(isinstance(metrics, list) and metrics, f"performance metric results missing: {job}")
            contract.append((
                comparison["key"],
                tuple((metric.get("name"), metric.get("direction"), metric.get("max_regression_ratio"),
                       metric.get("absolute_slack")) for metric in metrics if isinstance(metric, dict)),
            ))
        contract.sort()
        if common_performance_contract is None:
            common_performance_contract = contract
        else:
            require(contract == common_performance_contract, "Linux and Windows performance budgets differ")

        baseline_matrix_sha = verify_matrix_manifest(
            job_root,
            "performance-samples/baseline",
            PERFORMANCE_BASELINE_SHA,
            expected_platform,
            expected_backend,
            evidence_paths,
        )
        candidate_matrix_sha = verify_matrix_manifest(
            job_root,
            "performance-samples/candidate",
            job_manifest["checkout_sha"],
            expected_platform,
            expected_backend,
            evidence_paths,
        )

        platforms.append(
            {
                "job": job,
                "platform": expected_platform,
                "backend": expected_backend,
                "artifact_name": job_manifest["artifact_name"],
                "job_manifest_sha256": sha256_file(job_root / "job-manifest.json"),
                "semantic_manifest_sha256": sha256_file(semantic_path),
                "performance_regression_sha256": sha256_file(performance_path),
                "performance_baseline_matrix_sha256": baseline_matrix_sha,
                "performance_candidate_matrix_sha256": candidate_matrix_sha,
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
        "performance_baseline_sha": PERFORMANCE_BASELINE_SHA,
        "performance_comparisons": [item[0] for item in common_performance_contract or []],
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
