from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any


SCHEMA = "gamenet.ci_evidence_set.v1"
PRODUCER_SCHEMA = "gamenet.ci_evidence.v1"
INVENTORY_SCHEMA = "gamenet.ctest_inventory.v1"
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
EXPECTED_JOBS = (
    "linux-cmake",
    "linux-asan-ubsan",
    "linux-tsan",
    "linux-release",
    "windows-msvc",
    "windows-msvc-release",
)
CONSUMER_JOBS = frozenset({"linux-cmake", "windows-msvc", "windows-msvc-release"})
EXPECTED_MAIN_INVENTORY = 89
EXPECTED_THREADING_EXECUTION = 64
EXPECTED_LIBFUZZER_EXECUTIONS = 1000


class EvidenceSetError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceSetError(message)


def read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceSetError(f"cannot read {label} {path}: {error}") from error
    require(isinstance(document, dict), f"{label} must be a JSON object: {path}")
    return document


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_relative_path(raw_path: Any) -> PurePosixPath:
    require(isinstance(raw_path, str) and bool(raw_path), "evidence path must be non-empty")
    require("\\" not in raw_path, f"evidence path must use POSIX separators: {raw_path!r}")
    relative = PurePosixPath(raw_path)
    require(not relative.is_absolute(), f"evidence path must be relative: {raw_path!r}")
    require(
        all(part not in ("", ".", "..") for part in relative.parts),
        f"unsafe evidence path: {raw_path!r}",
    )
    return relative


def validate_hashed_files(artifact_dir: Path, manifest: dict[str, Any]) -> dict[str, Path]:
    entries = manifest.get("evidence_files")
    require(isinstance(entries, list), "producer manifest has no evidence_files array")

    declared: dict[str, Path] = {}
    declared_order: list[str] = []
    for entry in entries:
        require(isinstance(entry, dict), "evidence_files contains a non-object entry")
        relative = validate_relative_path(entry.get("path"))
        raw_path = relative.as_posix()
        require(raw_path not in declared, f"duplicate evidence file entry: {raw_path}")
        path = artifact_dir.joinpath(*relative.parts)
        require(path.is_file() and not path.is_symlink(), f"missing hashed evidence file: {raw_path}")
        expected_bytes = entry.get("bytes")
        expected_sha = entry.get("sha256")
        require(
            isinstance(expected_bytes, int) and not isinstance(expected_bytes, bool) and expected_bytes >= 0,
            f"invalid byte count for evidence file: {raw_path}",
        )
        require(
            isinstance(expected_sha, str) and re.fullmatch(r"[0-9a-f]{64}", expected_sha) is not None,
            f"invalid SHA-256 for evidence file: {raw_path}",
        )
        require(path.stat().st_size == expected_bytes, f"byte count mismatch for evidence file: {raw_path}")
        require(sha256_file(path) == expected_sha, f"SHA-256 mismatch for evidence file: {raw_path}")
        declared[raw_path] = path
        declared_order.append(raw_path)

    require(declared_order == sorted(declared_order), "producer evidence_files must be sorted by path")
    actual: set[str] = set()
    for path in artifact_dir.rglob("*"):
        require(not path.is_symlink(), f"producer artifact must not contain symlinks: {path}")
        if path.is_file() and path != artifact_dir / "manifest.json":
            actual.add(path.relative_to(artifact_dir).as_posix())
    require(actual == set(declared), "producer manifest file list does not match artifact contents")
    return declared


def inventory_tests(path: Path, expected_total: int) -> tuple[dict[str, list[str]], dict[str, Any]]:
    document = read_json(path, "CTest inventory")
    require(document.get("schema") == INVENTORY_SCHEMA, f"unexpected inventory schema: {path}")
    require(document.get("total") == expected_total, f"unexpected inventory total: {path}")
    require(document.get("expected_total") == expected_total, f"unexpected expected_total: {path}")
    tests = document.get("tests")
    require(isinstance(tests, list) and len(tests) == expected_total, f"inventory test list mismatch: {path}")
    labels_by_name: dict[str, list[str]] = {}
    for test in tests:
        require(isinstance(test, dict), f"inventory contains a non-object test: {path}")
        name = test.get("name")
        labels = test.get("labels")
        require(isinstance(name, str) and bool(name), f"inventory contains an unnamed test: {path}")
        require(name not in labels_by_name, f"inventory contains duplicate test: {name}")
        require(
            isinstance(labels, list) and all(isinstance(label, str) for label in labels),
            f"inventory labels are invalid for test: {name}",
        )
        labels_by_name[name] = labels
    return labels_by_name, document


def junit_tests(path: Path, expected_total: int) -> list[str]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise EvidenceSetError(f"cannot parse JUnit XML {path}: {error}") from error
    require(root.tag.rsplit("}", 1)[-1] == "testsuite", f"unexpected JUnit root: {path}")
    try:
        declared_total = int(root.attrib["tests"], 10)
        failures = int(root.attrib.get("failures", "0"), 10)
        errors = int(root.attrib.get("errors", "0"), 10)
        skipped = int(root.attrib.get("skipped", "0"), 10)
    except (KeyError, ValueError) as error:
        raise EvidenceSetError(f"invalid JUnit summary attributes: {path}") from error
    require(declared_total == expected_total, f"unexpected JUnit test count: {path}")
    require(failures == 0 and errors == 0 and skipped == 0, f"JUnit contains non-passing tests: {path}")
    require(root.find(".//failure") is None, f"JUnit contains a failure element: {path}")
    require(root.find(".//error") is None, f"JUnit contains an error element: {path}")
    require(root.find(".//skipped") is None, f"JUnit contains a skipped element: {path}")
    names = [case.attrib.get("name", "") for case in root.findall(".//testcase")]
    require(len(names) == expected_total, f"JUnit testcase list does not match summary: {path}")
    require(all(names) and len(set(names)) == len(names), f"JUnit test names are missing or duplicated: {path}")
    return names


def validate_identity(manifest: dict[str, Any], artifact_dir: Path) -> tuple[str, dict[str, Any]]:
    require(manifest.get("schema") == PRODUCER_SCHEMA, f"unexpected producer schema: {artifact_dir}")
    job = manifest.get("job")
    require(job in EXPECTED_JOBS, f"unexpected producer job: {job!r}")
    for key in ("checkout_sha", "candidate_sha", "github_sha", "merge_or_current_sha"):
        require(
            isinstance(manifest.get(key), str) and SHA_PATTERN.fullmatch(manifest[key]) is not None,
            f"invalid {key} for producer job {job}",
        )
    require(
        manifest["checkout_sha"] == manifest["github_sha"] == manifest["merge_or_current_sha"],
        f"checkout/GitHub/merge SHA mismatch for producer job {job}",
    )
    event_name = manifest.get("event_name")
    pr_head = manifest.get("pr_head_sha")
    if event_name == "pull_request":
        require(isinstance(pr_head, str) and SHA_PATTERN.fullmatch(pr_head) is not None, f"missing PR head for {job}")
        require(manifest["candidate_sha"] == pr_head, f"candidate/PR-head mismatch for producer job {job}")
    else:
        require(manifest["candidate_sha"] == manifest["github_sha"], f"non-PR candidate mismatch for {job}")

    run_id = manifest.get("run_id")
    run_attempt = manifest.get("run_attempt")
    require(isinstance(run_id, str) and run_id.isdecimal(), f"invalid run id for producer job {job}")
    require(
        isinstance(run_attempt, int) and not isinstance(run_attempt, bool) and run_attempt >= 1,
        f"invalid run attempt for producer job {job}",
    )
    artifact_name = manifest.get("artifact_name")
    expected_artifact_name = f"ci-evidence-{job}-{manifest['github_sha']}-{run_id}-{run_attempt}"
    require(artifact_name == expected_artifact_name, f"unexpected artifact name for producer job {job}")
    require(artifact_dir.name == artifact_name, f"download directory does not match artifact name for {job}")
    require(manifest.get("workflow") == "ci", f"unexpected workflow for producer job {job}")
    require(manifest.get("status") == "success", f"producer job did not succeed: {job}")
    require(manifest.get("pre_upload_status") == "success", f"producer pre-upload status is not success: {job}")
    require(manifest.get("status_scope") == "job_before_evidence_upload", f"unexpected status scope: {job}")

    identity = {
        key: manifest.get(key)
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
    }
    return job, identity


def validate_producer(artifact_dir: Path, manifest_path: Path) -> tuple[str, dict[str, Any], dict[str, Any]]:
    manifest = read_json(manifest_path, "producer manifest")
    job, identity = validate_identity(manifest, artifact_dir)
    files = validate_hashed_files(artifact_dir, manifest)
    for required in ("toolchain.txt", "ctest-inventory.json", "ctest-junit.xml", "ctest.log"):
        require(required in files, f"producer job {job} is missing required evidence: {required}")

    main_inventory, inventory_document = inventory_tests(files["ctest-inventory.json"], EXPECTED_MAIN_INVENTORY)
    if job == "linux-tsan":
        require(
            inventory_document.get("label_counts", {}).get("threading") == EXPECTED_THREADING_EXECUTION,
            "TSan inventory does not contain exactly 64 threading tests",
        )
        require(
            inventory_document.get("expected_label_counts", {}).get("threading") == EXPECTED_THREADING_EXECUTION,
            "TSan inventory did not enforce threading=64",
        )
        expected_main_names = {name for name, labels in main_inventory.items() if "threading" in labels}
        expected_main_total = EXPECTED_THREADING_EXECUTION
    else:
        expected_main_names = set(main_inventory)
        expected_main_total = EXPECTED_MAIN_INVENTORY
    main_junit_names = junit_tests(files["ctest-junit.xml"], expected_main_total)
    require(set(main_junit_names) == expected_main_names, f"JUnit selection does not match inventory for {job}")

    consumer_total: int | None = None
    consumer_paths = {
        "install-consumer-inventory.json",
        "install-consumer-junit.xml",
        "install-consumer-ctest.log",
    }
    if job in CONSUMER_JOBS:
        require(consumer_paths <= set(files), f"producer job {job} is missing install-consumer evidence")
        consumer_inventory, _ = inventory_tests(files["install-consumer-inventory.json"], 1)
        consumer_junit_names = junit_tests(files["install-consumer-junit.xml"], 1)
        require(set(consumer_junit_names) == set(consumer_inventory), f"consumer JUnit mismatch for {job}")
        consumer_total = 1
    else:
        require(not (consumer_paths & set(files)), f"unexpected install-consumer evidence for {job}")

    fuzz_executions: int | None = None
    if job == "linux-asan-ubsan":
        require("fuzz/fuzzer.log" in files, "ASan producer is missing the libFuzzer log")
        require("fuzz/packet_framer.dict" in files, "ASan producer is missing the fuzz dictionary")
        require(
            any(name.startswith("fuzz/corpus/") for name in files),
            "ASan producer is missing the fuzz corpus",
        )
        fuzz_log = files["fuzz/fuzzer.log"].read_text(encoding="utf-8", errors="replace")
        execution_matches = re.findall(
            r"(?m)^stat::number_of_executed_units:\s*(\d+)\s*$", fuzz_log
        )
        require(
            len(execution_matches) == 1,
            "ASan libFuzzer log must contain exactly one executed-unit statistic",
        )
        fuzz_executions = int(execution_matches[0])
        require(
            fuzz_executions == EXPECTED_LIBFUZZER_EXECUTIONS,
            f"ASan libFuzzer execution count mismatch: expected "
            f"{EXPECTED_LIBFUZZER_EXECUTIONS}, got {fuzz_executions}",
        )
        require(
            re.search(r"(?m)^#1000\s+DONE\b", fuzz_log) is not None,
            "ASan libFuzzer log lacks the exact #1000 DONE marker",
        )

    summary = {
        "job": job,
        "artifact_name": manifest["artifact_name"],
        "producer_manifest_sha256": sha256_file(manifest_path),
        "evidence_file_count": len(files),
        "evidence_bytes": sum(path.stat().st_size for path in files.values()),
        "configured_tests": EXPECTED_MAIN_INVENTORY,
        "executed_tests": expected_main_total,
        "consumer_executed_tests": consumer_total,
        "fuzz_executed_units": fuzz_executions,
    }
    return job, identity, summary


def verify_evidence_set(input_root: Path, output: Path) -> dict[str, Any]:
    root = input_root.resolve()
    require(root.is_dir(), f"evidence-set input root does not exist: {input_root}")
    require(not input_root.is_symlink(), "evidence-set input root must not be a symlink")
    try:
        output.resolve().relative_to(root)
    except ValueError:
        pass
    else:
        raise EvidenceSetError("aggregate output must be outside the producer input root")

    artifact_dirs = sorted(root.iterdir(), key=lambda path: path.as_posix())
    require(
        len(artifact_dirs) == len(EXPECTED_JOBS)
        and all(path.is_dir() and not path.is_symlink() for path in artifact_dirs),
        "evidence set must contain exactly six producer artifact directories",
    )
    manifest_paths = [path / "manifest.json" for path in artifact_dirs]
    require(
        all(path.is_file() and not path.is_symlink() for path in manifest_paths),
        "each producer artifact directory must contain one direct manifest.json",
    )

    identities: list[dict[str, Any]] = []
    summaries: dict[str, dict[str, Any]] = {}
    for manifest_path in manifest_paths:
        artifact_dir = manifest_path.parent
        job, identity, summary = validate_producer(artifact_dir, manifest_path)
        require(job not in summaries, f"duplicate producer job: {job}")
        identities.append(identity)
        summaries[job] = summary
    require(set(summaries) == set(EXPECTED_JOBS), "evidence set does not contain the exact six producer jobs")
    common_identity = identities[0]
    for identity in identities[1:]:
        require(identity == common_identity, "producer manifests do not share one run/attempt/SHA identity")

    artifact_name = (
        f"ci-evidence-set-{common_identity['github_sha']}-"
        f"{common_identity['run_id']}-{common_identity['run_attempt']}"
    )
    generated_at = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    aggregate = {
        "schema": SCHEMA,
        "generated_at_utc": generated_at,
        "date_utc": generated_at[:10],
        "status": "success",
        "artifact_name": artifact_name,
        **common_identity,
        "expected_jobs": list(EXPECTED_JOBS),
        "producers": [summaries[job] for job in EXPECTED_JOBS],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(aggregate, indent=2) + "\n", encoding="utf-8")
    return aggregate


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify and aggregate the six main CI evidence artifacts.")
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        aggregate = verify_evidence_set(arguments.input_root, arguments.output)
    except (EvidenceSetError, OSError) as error:
        print(f"CI evidence-set verification failed: {error}", file=sys.stderr)
        return 1
    print(
        f"verified {len(aggregate['producers'])} CI producer artifacts for "
        f"{aggregate['github_sha']} run {aggregate['run_id']} attempt {aggregate['run_attempt']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
