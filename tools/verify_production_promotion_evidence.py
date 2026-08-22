from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

import verify_capacity_gate_evidence_set
from verify_endurance_evidence import (
    EvidenceError as EnduranceEvidenceError,
    load_and_verify as load_and_verify_endurance,
)


SCHEMA = "gamenet.production_promotion_evidence.v2"
WAIVER_SCHEMA = "gamenet.production_promotion_waiver.v2"
STAGE_CAPACITY_PROFILES = {
    "candidate": "candidate-10k",
    "release": "dedicated-100k",
}
STAGE_CURRENT_ENDURANCE_MODES = {
    "candidate": "candidate-1h",
    "release": "release-3h",
}
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")


class PromotionEvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PromotionEvidenceError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def positive_run_id(value: Any, label: str) -> str:
    require(
        isinstance(value, str)
        and value.isdecimal()
        and int(value, 10) > 0,
        f"{label} must be a positive decimal workflow run ID",
    )
    return value


def positive_attempt(value: Any, label: str) -> int:
    require(
        isinstance(value, int)
        and not isinstance(value, bool)
        and value > 0,
        f"{label} must be a positive workflow run attempt",
    )
    return value


def load_json(path: Path, label: str) -> dict[str, Any]:
    require(path.is_file() and not path.is_symlink(), f"{label} is missing")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PromotionEvidenceError(
            f"cannot read {label} {path}: {error}"
        ) from error
    require(isinstance(document, dict), f"{label} must be a JSON object")
    return document


def verify_endurance(
    path: Path,
    *,
    mode: str,
    candidate_sha: str,
    workflow_run_id: str,
    workflow_run_attempt: int,
    role: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    require(
        path.is_file() and not path.is_symlink(),
        f"{role} endurance evidence is missing",
    )
    resolved = path.resolve()
    document = load_and_verify_endurance(
        resolved,
        mode,
        candidate_sha,
        "linux",
        "epoll",
        workflow_run_id=workflow_run_id,
        workflow_run_attempt=workflow_run_attempt,
    )
    log = document.get("log")
    require(isinstance(log, dict), f"{role} endurance log is missing")
    log_name = log.get("file")
    require(isinstance(log_name, str), f"{role} endurance log name is invalid")
    log_path = resolved.parent / log_name
    require(not log_path.is_symlink(), f"{role} endurance log must not be a symlink")
    return document, {
        "role": role,
        "mode": mode,
        "workflow_run_id": workflow_run_id,
        "workflow_run_attempt": workflow_run_attempt,
        "result_file": resolved.name,
        "result_sha256": sha256_file(resolved),
        "log_file": log_name,
        "log_sha256": log["sha256"],
        "completed_cycles": document["completed_cycles"],
        "elapsed_milliseconds": document["child_elapsed_milliseconds"],
    }


def verify_promotion(
    *,
    stage: str,
    capacity_root: Path,
    endurance_evidence: Path | None,
    candidate_sha: str,
    capacity_run_id: str,
    capacity_run_attempt: int,
    promotion_run_id: str,
    promotion_run_attempt: int,
    candidate_endurance_evidence: Path | None = None,
    candidate_endurance_run_id: str | None = None,
    candidate_endurance_run_attempt: int | None = None,
    waive_endurance: bool = False,
    waiver_reason: str | None = None,
    waiver_approved_by: str | None = None,
) -> dict[str, Any]:
    require(stage in STAGE_CAPACITY_PROFILES, "unsupported promotion stage")
    require(
        SHA_PATTERN.fullmatch(candidate_sha) is not None,
        "candidate SHA must be 40 lowercase hex characters",
    )
    capacity_run_id = positive_run_id(
        capacity_run_id,
        "capacity source",
    )
    capacity_run_attempt = positive_attempt(
        capacity_run_attempt,
        "capacity source",
    )
    promotion_run_id = positive_run_id(
        promotion_run_id,
        "promotion source",
    )
    promotion_run_attempt = positive_attempt(
        promotion_run_attempt,
        "promotion source",
    )
    if waive_endurance:
        require(
            endurance_evidence is None,
            "endurance waiver must not claim current endurance evidence",
        )
        require(
            candidate_endurance_evidence is None
            and candidate_endurance_run_id is None
            and candidate_endurance_run_attempt is None,
            "endurance waiver must not claim prior endurance evidence",
        )
        require(
            isinstance(waiver_reason, str)
            and 12 <= len(waiver_reason.strip()) <= 500
            and "\n" not in waiver_reason
            and "\r" not in waiver_reason,
            "endurance waiver reason must be a 12-500 character single line",
        )
        require(
            isinstance(waiver_approved_by, str)
            and 1 <= len(waiver_approved_by.strip()) <= 100
            and "\n" not in waiver_approved_by
            and "\r" not in waiver_approved_by,
            "endurance waiver approver must be a 1-100 character single line",
        )
        waiver_reason = waiver_reason.strip()
        waiver_approved_by = waiver_approved_by.strip()
    else:
        require(
            waiver_reason is None and waiver_approved_by is None,
            "waiver metadata requires --waive-endurance",
        )

    require(
        capacity_root.is_dir() and not capacity_root.is_symlink(),
        "capacity evidence root is missing",
    )
    resolved_capacity_root = capacity_root.resolve()
    capacity_pair = (
        verify_capacity_gate_evidence_set.verify_evidence_set(
            resolved_capacity_root
        )
    )
    pair_manifest_path = resolved_capacity_root / "pair-manifest.json"
    pair_manifest = load_json(
        pair_manifest_path,
        "capacity pair manifest",
    )
    require(
        pair_manifest == capacity_pair,
        "capacity pair manifest does not match revalidated raw evidence",
    )
    require(
        capacity_pair.get("profile")
        == STAGE_CAPACITY_PROFILES[stage],
        "capacity profile does not satisfy the promotion stage",
    )
    require(
        capacity_pair.get("candidate_sha") == candidate_sha,
        "capacity candidate SHA does not match promotion identity",
    )
    require(
        capacity_pair.get("run_id") == capacity_run_id,
        "capacity workflow run ID does not match promotion input",
    )
    require(
        capacity_pair.get("run_attempt") == capacity_run_attempt,
        "capacity workflow run attempt does not match promotion input",
    )

    capacity_entry = {
        "profile": capacity_pair["profile"],
        "workflow_run_id": capacity_run_id,
        "workflow_run_attempt": capacity_run_attempt,
        "pair_manifest_file": pair_manifest_path.name,
        "pair_manifest_sha256": sha256_file(pair_manifest_path),
        "endpoint_attempts": capacity_pair["endpoint_attempts"],
        "probe_attempts": capacity_pair["probe_attempts"],
        "platforms": capacity_pair["platforms"],
    }
    if waive_endurance:
        return {
            "schema": WAIVER_SCHEMA,
            "status": "waived",
            "stage": stage,
            "candidate_sha": candidate_sha,
            "promotion_source": {
                "workflow_run_id": promotion_run_id,
                "workflow_run_attempt": promotion_run_attempt,
            },
            "capacity": capacity_entry,
            "endurance_policy": "owner-waived",
            "endurance": [],
            "waiver": {
                "scope": (
                    "candidate-1h"
                    if stage == "candidate"
                    else "candidate-1h+release-3h"
                ),
                "approved_by": waiver_approved_by,
                "reason": waiver_reason,
                "duration_evidence_complete": False,
                "owner_authorized_promotion": True,
            },
        }

    endurance_entries: list[dict[str, Any]] = []
    if stage == "candidate":
        require(
            candidate_endurance_evidence is None
            and candidate_endurance_run_id is None
            and candidate_endurance_run_attempt is None,
            "candidate promotion must not claim prior endurance evidence",
        )
    else:
        require(
            candidate_endurance_evidence is not None,
            "release promotion requires candidate-1h evidence",
        )
        candidate_run_id = positive_run_id(
            candidate_endurance_run_id,
            "candidate endurance source",
        )
        candidate_run_attempt = positive_attempt(
            candidate_endurance_run_attempt,
            "candidate endurance source",
        )
        _, candidate_entry = verify_endurance(
            candidate_endurance_evidence,
            mode="candidate-1h",
            candidate_sha=candidate_sha,
            workflow_run_id=candidate_run_id,
            workflow_run_attempt=candidate_run_attempt,
            role="candidate",
        )
        endurance_entries.append(candidate_entry)

    require(
        endurance_evidence is not None,
        "promotion requires current endurance evidence",
    )
    _, current_entry = verify_endurance(
        endurance_evidence,
        mode=STAGE_CURRENT_ENDURANCE_MODES[stage],
        candidate_sha=candidate_sha,
        workflow_run_id=promotion_run_id,
        workflow_run_attempt=promotion_run_attempt,
        role=stage,
    )
    endurance_entries.append(current_entry)

    return {
        "schema": SCHEMA,
        "status": "success",
        "stage": stage,
        "candidate_sha": candidate_sha,
        "promotion_source": {
            "workflow_run_id": promotion_run_id,
            "workflow_run_attempt": promotion_run_attempt,
        },
        "capacity": capacity_entry,
        "endurance": endurance_entries,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Revalidate and bind GameNet capacity/endurance promotion evidence"
        )
    )
    parser.add_argument(
        "--stage",
        choices=tuple(STAGE_CAPACITY_PROFILES),
        required=True,
    )
    parser.add_argument("--capacity-root", type=Path, required=True)
    parser.add_argument("--endurance-evidence", type=Path)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--capacity-run-id", required=True)
    parser.add_argument("--capacity-run-attempt", type=int, required=True)
    parser.add_argument("--promotion-run-id", required=True)
    parser.add_argument("--promotion-run-attempt", type=int, required=True)
    parser.add_argument("--candidate-endurance-evidence", type=Path)
    parser.add_argument("--candidate-endurance-run-id")
    parser.add_argument("--candidate-endurance-run-attempt", type=int)
    parser.add_argument("--waive-endurance", action="store_true")
    parser.add_argument("--waiver-reason")
    parser.add_argument("--waiver-approved-by")
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        require(
            not arguments.output.exists(),
            "promotion output already exists",
        )
        document = verify_promotion(
            stage=arguments.stage,
            capacity_root=arguments.capacity_root,
            endurance_evidence=arguments.endurance_evidence,
            candidate_sha=arguments.candidate_sha,
            capacity_run_id=arguments.capacity_run_id,
            capacity_run_attempt=arguments.capacity_run_attempt,
            promotion_run_id=arguments.promotion_run_id,
            promotion_run_attempt=arguments.promotion_run_attempt,
            candidate_endurance_evidence=(
                arguments.candidate_endurance_evidence
            ),
            candidate_endurance_run_id=(
                arguments.candidate_endurance_run_id
            ),
            candidate_endurance_run_attempt=(
                arguments.candidate_endurance_run_attempt
            ),
            waive_endurance=arguments.waive_endurance,
            waiver_reason=arguments.waiver_reason,
            waiver_approved_by=arguments.waiver_approved_by,
        )
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except (
        OSError,
        PromotionEvidenceError,
        EnduranceEvidenceError,
        verify_capacity_gate_evidence_set.CapacityGatePairError,
    ) as error:
        print(
            f"production promotion evidence verification failed: {error}",
            file=sys.stderr,
        )
        return 1
    outcome = "waived" if document["status"] == "waived" else "passed"
    print(
        f"production {document['stage']} promotion evidence {outcome}: "
        f"{document['candidate_sha']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
