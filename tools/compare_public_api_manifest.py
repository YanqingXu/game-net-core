#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


DIFF_SCHEMA = "gamenet.public_api_diff.v1"
MANIFEST_SCHEMA = "gamenet.public_api_manifest.v2"
SNAPSHOT_SCHEMA = "gamenet.public_api_snapshot.v1"
CATEGORIES = ("platform_internal", "provisional", "stable_core")


def load_document(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return document


def _inventory(document: dict[str, Any], section: str) -> dict[str, str]:
    groups = document.get(section)
    if not isinstance(groups, dict) or set(groups) != set(CATEGORIES):
        raise ValueError(f"{section} must contain exactly: {', '.join(CATEGORIES)}")
    inventory: dict[str, str] = {}
    for category in CATEGORIES:
        values = groups[category]
        if (
            not isinstance(values, list)
            or not all(isinstance(value, str) for value in values)
            or values != sorted(values)
            or len(values) != len(set(values))
        ):
            raise ValueError(f"{section}.{category} must be a sorted unique string list")
        for value in values:
            if value in inventory:
                raise ValueError(f"{section} categories overlap at {value}")
            inventory[value] = category
    return inventory


def _inventory_diff(
    baseline: dict[str, str],
    candidate: dict[str, str],
) -> dict[str, list[dict[str, str]]]:
    added = [
        {"name": name, "category": candidate[name]}
        for name in sorted(set(candidate) - set(baseline))
    ]
    removed = [
        {"name": name, "category": baseline[name]}
        for name in sorted(set(baseline) - set(candidate))
    ]
    category_moves = [
        {"name": name, "from": baseline[name], "to": candidate[name]}
        for name in sorted(set(baseline) & set(candidate))
        if baseline[name] != candidate[name]
    ]
    return {
        "added": added,
        "removed": removed,
        "category_moves": category_moves,
    }


def _fingerprint_changes(
    baseline: dict[str, Any],
    candidate: dict[str, Any],
) -> list[dict[str, str]]:
    before = baseline.get("stable_header_fingerprints")
    after = candidate.get("stable_header_fingerprints")
    if not isinstance(before, dict) or not isinstance(after, dict):
        raise ValueError("both documents must contain stable_header_fingerprints maps")
    return [
        {"header": header, "before": before[header], "after": after[header]}
        for header in sorted(set(before) & set(after))
        if before[header] != after[header]
    ]


def _identity(
    document: dict[str, Any],
    *,
    path: str,
) -> dict[str, Any]:
    schema = document.get("schema")
    identity: dict[str, Any] = {
        "path": path,
        "schema": schema,
        "package_version": document.get("package_version"),
        "compatibility_line": document.get("compatibility_line"),
    }
    if schema == SNAPSHOT_SCHEMA:
        source = document.get("source")
        if not isinstance(source, dict):
            raise ValueError("baseline snapshot must contain source metadata")
        identity["tag"] = source.get("tag")
        identity["commit"] = source.get("commit")
    elif schema == MANIFEST_SCHEMA:
        identity["release_label"] = document.get("release_label")
    else:
        raise ValueError(f"unsupported API document schema: {schema!r}")
    return identity


def _stable_review_required(
    target_changes: dict[str, list[dict[str, str]]],
    header_changes: dict[str, list[dict[str, str]]],
    fingerprint_changes: list[dict[str, str]],
) -> bool:
    for changes in (target_changes, header_changes):
        if any(item["category"] == "stable_core" for item in changes["added"]):
            return True
        if any(item["category"] == "stable_core" for item in changes["removed"]):
            return True
        if any(
            item["from"] == "stable_core" or item["to"] == "stable_core"
            for item in changes["category_moves"]
        ):
            return True
    return bool(fingerprint_changes)


def _decision_reasons(
    target_changes: dict[str, list[dict[str, str]]],
    header_changes: dict[str, list[dict[str, str]]],
    fingerprint_changes: list[dict[str, str]],
) -> list[str]:
    reasons: list[str] = []
    reasons.extend(
        f"stable target removed: {item['name']}"
        for item in target_changes["removed"]
        if item["category"] == "stable_core"
    )
    reasons.extend(
        f"stable target moved to {item['to']}: {item['name']}"
        for item in target_changes["category_moves"]
        if item["from"] == "stable_core"
    )
    reasons.extend(
        f"stable header removed: {item['name']}"
        for item in header_changes["removed"]
        if item["category"] == "stable_core"
    )
    reasons.extend(
        f"stable header moved to {item['to']}: {item['name']}"
        for item in header_changes["category_moves"]
        if item["from"] == "stable_core"
    )
    reasons.extend(
        f"stable header fingerprint changed: {item['header']}"
        for item in fingerprint_changes
    )
    return sorted(reasons)


def build_diff(
    baseline: dict[str, Any],
    candidate: dict[str, Any],
    *,
    baseline_path: str,
    candidate_path: str,
    verify_historical_reference: bool = True,
) -> dict[str, Any]:
    if baseline.get("schema") != SNAPSHOT_SCHEMA:
        raise ValueError(f"baseline schema must be {SNAPSHOT_SCHEMA}")
    if candidate.get("schema") != MANIFEST_SCHEMA:
        raise ValueError(f"candidate schema must be {MANIFEST_SCHEMA}")

    source = baseline.get("source")
    if not isinstance(source, dict):
        raise ValueError("baseline source metadata is required")
    if verify_historical_reference:
        reference = candidate.get("historical_baseline")
        if not isinstance(reference, dict):
            raise ValueError("candidate historical baseline reference is required")
        if reference.get("tag") != source.get("tag") or reference.get("commit") != source.get(
            "commit"
        ):
            raise ValueError("candidate historical baseline reference does not match baseline source")
        if reference.get("path") != baseline_path:
            raise ValueError(
                f"candidate historical baseline path is {reference.get('path')!r}, "
                f"not {baseline_path!r}"
            )

    baseline_targets = _inventory(baseline, "targets")
    candidate_targets = _inventory(candidate, "targets")
    baseline_headers = _inventory(baseline, "headers")
    candidate_headers = _inventory(candidate, "headers")
    target_changes = _inventory_diff(baseline_targets, candidate_targets)
    header_changes = _inventory_diff(baseline_headers, candidate_headers)
    fingerprint_changes = _fingerprint_changes(baseline, candidate)
    same_line = baseline.get("compatibility_line") == candidate.get("compatibility_line")
    reasons = _decision_reasons(target_changes, header_changes, fingerprint_changes)
    stable_review_required = _stable_review_required(
        target_changes, header_changes, fingerprint_changes
    )
    has_changes = any(target_changes.values()) or any(header_changes.values()) or bool(
        fingerprint_changes
    )

    return {
        "schema": DIFF_SCHEMA,
        "baseline": _identity(baseline, path=baseline_path),
        "candidate": _identity(candidate, path=candidate_path),
        "changes": {
            "targets": target_changes,
            "headers": header_changes,
            "stable_header_fingerprint_changes": fingerprint_changes,
        },
        "summary": {
            "has_changes": has_changes,
            "same_compatibility_line": same_line,
            "stable_surface_review_required": stable_review_required,
            "compatibility_decision_required": same_line and bool(reasons),
            "compatibility_decision_reasons": reasons if same_line else [],
        },
    }


def render_diff(document: dict[str, Any]) -> str:
    return json.dumps(document, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare the current public API manifest with its immutable historical snapshot"
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument(
        "--compatibility-baseline",
        type=Path,
        help="same-line reviewed snapshot used only by the blocking compatibility gate",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--compatibility-output",
        type=Path,
        help="write the same-line compatibility-baseline diff separately",
    )
    parser.add_argument("--fail-on-compatibility-decision", action="store_true")
    parser.add_argument("--fail-on-stable-surface-review", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    candidate_path = args.candidate or repo_root / "api" / "public_api_manifest.json"
    try:
        candidate = load_document(candidate_path)
        reference = candidate.get("historical_baseline")
        if not isinstance(reference, dict) or not isinstance(reference.get("path"), str):
            raise ValueError("candidate historical_baseline.path is required")
        baseline_relative = reference["path"]
        baseline_path = args.baseline or repo_root / baseline_relative
        if args.baseline is not None:
            try:
                baseline_relative = baseline_path.resolve().relative_to(repo_root).as_posix()
            except ValueError as error:
                raise ValueError("--baseline must be inside --repo-root") from error
        candidate_relative = candidate_path.resolve().relative_to(repo_root).as_posix()
        baseline = load_document(baseline_path)
        difference = build_diff(
            baseline,
            candidate,
            baseline_path=baseline_relative,
            candidate_path=candidate_relative,
        )
    except (OSError, ValueError) as error:
        print(f"public API comparison error: {error}", file=sys.stderr)
        return 1

    rendered = render_diff(difference)
    if args.output is None:
        sys.stdout.write(rendered)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
        print(f"public API diff written: {args.output}")

    gate_difference = difference
    if args.compatibility_output is not None and args.compatibility_baseline is None:
        print(
            "public API compatibility output requires --compatibility-baseline",
            file=sys.stderr,
        )
        return 1
    if args.compatibility_baseline is not None:
        compatibility_path = args.compatibility_baseline
        if not compatibility_path.is_absolute():
            compatibility_path = repo_root / compatibility_path
        try:
            compatibility_relative = compatibility_path.resolve().relative_to(
                repo_root
            ).as_posix()
            compatibility_baseline = load_document(compatibility_path)
            gate_difference = build_diff(
                compatibility_baseline,
                candidate,
                baseline_path=compatibility_relative,
                candidate_path=candidate_relative,
                verify_historical_reference=False,
            )
            if not gate_difference["summary"]["same_compatibility_line"]:
                raise ValueError(
                    "compatibility baseline must use the candidate compatibility line"
                )
        except (OSError, ValueError) as error:
            print(f"public API compatibility gate error: {error}", file=sys.stderr)
            return 1

    if args.compatibility_output is not None:
        compatibility_rendered = render_diff(gate_difference)
        args.compatibility_output.parent.mkdir(parents=True, exist_ok=True)
        args.compatibility_output.write_text(
            compatibility_rendered,
            encoding="utf-8",
        )
        print(f"public API compatibility diff written: {args.compatibility_output}")

    if (
        args.fail_on_stable_surface_review
        and gate_difference["summary"]["stable_surface_review_required"]
    ):
        print("public API comparison requires stable-surface review", file=sys.stderr)
        return 3

    if (
        args.fail_on_compatibility_decision
        and gate_difference["summary"]["compatibility_decision_required"]
    ):
        print("public API comparison requires a compatibility decision", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
