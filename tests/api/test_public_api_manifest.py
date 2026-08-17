from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def load_tool(repo_root: Path, name: str):
    tool_path = repo_root / "tools" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, tool_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def candidate_from_snapshot(snapshot: dict, baseline_reference: dict) -> dict:
    candidate = copy.deepcopy(snapshot)
    candidate["schema"] = "gamenet.public_api_manifest.v2"
    candidate["release_label"] = "v0.2.0-production-candidate"
    candidate["historical_baseline"] = copy.deepcopy(baseline_reference)
    candidate.pop("source")
    return candidate


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    manifest_path = repo_root / "api" / "public_api_manifest.json"
    verifier = load_tool(repo_root, "verify_public_api_manifest")
    comparer = load_tool(repo_root, "compare_public_api_manifest")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    baseline_reference = manifest["historical_baseline"]
    baseline_path = repo_root / baseline_reference["path"]
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    reviewed_snapshot_path = (
        repo_root / "api" / "baselines" / "v0.3.0-perf-r1-reviewed.json"
    )
    reviewed_snapshot = json.loads(reviewed_snapshot_path.read_text(encoding="utf-8"))
    prior_reviewed_snapshot = json.loads(
        (
            repo_root
            / "api"
            / "baselines"
            / "v0.3.0-api-r1-reviewed.json"
        ).read_text(encoding="utf-8")
    )

    errors = verifier.verify_manifest(
        repo_root,
        manifest_path,
        require_git_baseline=True,
    )
    assert not errors, "\n".join(errors)
    assert "include/gamenet/core/metrics/MetricsExporter.h" in manifest["headers"]["provisional"]
    assert "include/gamenet/core/metrics/MetricsHookRecorder.h" in manifest["headers"]["provisional"]
    assert "include/gamenet/core/metrics/MetricsExporter.h" not in manifest[
        "stable_header_fingerprints"
    ]
    assert "include/gamenet/core/metrics/MetricsHookRecorder.h" not in manifest[
        "stable_header_fingerprints"
    ]
    assert manifest["schema"] == "gamenet.public_api_manifest.v2"
    assert manifest["compatibility_line"] == "0.3"
    assert manifest["targets"]["stable_core"] == ["GameNet::core"]
    assert manifest["targets"]["platform_internal"] == []
    assert "GameNet::protocol" in manifest["targets"]["provisional"]
    assert "include/gamenet/core/metrics/MetricsExporter.h" in manifest["headers"][
        "provisional"
    ]
    assert "include/gamenet/core/metrics/MetricsExporter.h" not in manifest["headers"][
        "stable_core"
    ]
    assert baseline["source"] == {
        "tag": "v0.2.0-phase4-preview",
        "commit": "7668d6b82a0d815ccd79f83c572bc0a36bcceea0",
    }
    assert reviewed_snapshot["schema"] == "gamenet.public_api_snapshot.v1"
    assert reviewed_snapshot["package_version"] == "0.3.0"
    assert reviewed_snapshot["compatibility_line"] == "0.3"
    assert reviewed_snapshot["source"] == {
        "tag": "api-r1-perf-r1-reviewed-surface",
        "commit": "6b292156e3e94d3389e9f3b8513445e7eb4ab541",
    }
    reviewed_provenance_errors: list[str] = []
    verifier._verify_snapshot_against_git(
        repo_root,
        reviewed_snapshot,
        reviewed_snapshot["source"]["commit"],
        reviewed_provenance_errors,
    )
    assert reviewed_provenance_errors == []
    reviewed_difference = comparer.build_diff(
        reviewed_snapshot,
        manifest,
        baseline_path="api/baselines/v0.3.0-perf-r1-reviewed.json",
        candidate_path="api/public_api_manifest.json",
        verify_historical_reference=False,
    )
    assert reviewed_difference["summary"]["same_compatibility_line"] is True
    assert reviewed_difference["summary"]["has_changes"] is False
    assert reviewed_difference["summary"]["compatibility_decision_required"] is False
    changed_candidate = copy.deepcopy(manifest)
    changed_reviewed_header = reviewed_snapshot["headers"]["stable_core"][0]
    changed_candidate["stable_header_fingerprints"][changed_reviewed_header] = "0" * 64
    changed_reviewed_difference = comparer.build_diff(
        reviewed_snapshot,
        changed_candidate,
        baseline_path="api/baselines/v0.3.0-perf-r1-reviewed.json",
        candidate_path="api/public_api_manifest.json",
        verify_historical_reference=False,
    )
    assert changed_reviewed_difference["summary"]["compatibility_decision_required"] is True
    additive_difference = comparer.build_diff(
        prior_reviewed_snapshot,
        manifest,
        baseline_path="api/baselines/v0.3.0-api-r1-reviewed.json",
        candidate_path="api/public_api_manifest.json",
        verify_historical_reference=False,
    )
    assert additive_difference["summary"] == {
        "has_changes": True,
        "same_compatibility_line": True,
        "stable_surface_review_required": True,
        "compatibility_decision_required": True,
        "compatibility_decision_reasons": [
            "stable header fingerprint changed: "
            "include/gamenet/core/net/TcpConnection.h"
        ],
    }
    assert additive_difference["changes"][
        "stable_header_fingerprint_changes"
    ] == [
        {
            "header": "include/gamenet/core/net/TcpConnection.h",
            "before": "3569bb5482922df1419843c549b36786d424691de9296afbbe36e5689cf5b381",
            "after": "86430ee090250e014d47336ab2a0e5ffbdf0c9a4ba555e6c8e8d04788b5e7552",
        }
    ]
    archived_additive_difference = json.loads(
        (
            repo_root
            / "docs"
            / "reviews"
            / "perf-r1-public-api-additive-diff.json"
        ).read_text(encoding="utf-8")
    )
    assert archived_additive_difference == additive_difference
    baseline_text = baseline_path.read_text(encoding="utf-8")
    assert (
        verifier.snapshot_content_sha256(baseline_text)
        == baseline_reference["snapshot_sha256"]
    )
    assert verifier.snapshot_content_sha256(
        baseline_text.replace("\n", "\r\n")
    ) == verifier.snapshot_content_sha256(baseline_text)
    tampered_baseline = copy.deepcopy(baseline)
    tampered_header = tampered_baseline["headers"]["stable_core"][0]
    tampered_baseline["stable_header_fingerprints"][tampered_header] = "0" * 64
    provenance_errors: list[str] = []
    verifier._verify_snapshot_against_git(
        repo_root,
        tampered_baseline,
        baseline["source"]["commit"],
        provenance_errors,
    )
    assert any(
        "stable fingerprint does not match" in error
        for error in provenance_errors
    )

    historical_candidate = candidate_from_snapshot(
        prior_reviewed_snapshot,
        baseline_reference,
    )
    historical_candidate["release_label"] = "v0.3.0-production-candidate"
    difference = comparer.build_diff(
        baseline,
        historical_candidate,
        baseline_path=baseline_reference["path"],
        candidate_path="api/public_api_manifest.json",
    )
    current_difference = comparer.build_diff(
        baseline,
        manifest,
        baseline_path=baseline_reference["path"],
        candidate_path="api/public_api_manifest.json",
    )
    assert difference["schema"] == "gamenet.public_api_diff.v1"
    assert difference["baseline"]["tag"] == "v0.2.0-phase4-preview"
    assert (
        difference["baseline"]["commit"]
        == "7668d6b82a0d815ccd79f83c572bc0a36bcceea0"
    )
    assert difference["summary"]["has_changes"] is True
    assert difference["summary"]["same_compatibility_line"] is False
    assert difference["summary"]["stable_surface_review_required"] is True
    assert difference["summary"]["compatibility_decision_required"] is False
    added_headers = {
        item["name"]: item["category"]
        for item in difference["changes"]["headers"]["added"]
    }
    assert sum(category == "stable_core" for category in added_headers.values()) == 10
    assert sum(category == "provisional" for category in added_headers.values()) == 4
    assert len(difference["changes"]["stable_header_fingerprint_changes"]) == 19
    assert difference["changes"]["headers"]["removed"] == []
    assert difference["changes"]["headers"]["category_moves"] == []
    assert difference["changes"]["targets"] == {
        "added": [],
        "removed": [],
        "category_moves": [],
    }
    assert added_headers["include/gamenet/core/net/CallbackException.h"] == "stable_core"
    assert added_headers["include/gamenet/core/net/TcpConnectionOptions.h"] == "stable_core"
    assert (
        added_headers["include/gamenet/core/net/TcpOutputMemoryBudget.h"]
        == "stable_core"
    )
    assert added_headers["include/gamenet/core/metrics/MetricsExporter.h"] == "provisional"
    assert difference["changes"]["stable_header_fingerprint_changes"]
    archived_difference = json.loads(
        (repo_root / "docs" / "reviews" / "api-r1-public-api-diff.json").read_text(
            encoding="utf-8"
        )
    )
    assert archived_difference == difference
    archived_compatibility_difference = json.loads(
        (
            repo_root
            / "docs"
            / "reviews"
            / "perf-r1-public-api-compatibility-diff.json"
        ).read_text(encoding="utf-8")
    )
    assert archived_compatibility_difference == reviewed_difference
    review_packet = (
        repo_root / "docs" / "reviews" / "api-r1-stable-core-review.md"
    ).read_text(encoding="utf-8")
    assert "/root/api_r1_independent_review" in review_packet
    assert "api-r1-approved-surface" in review_packet
    assert "v0.3.0-rel-c1-refreeze-1" in review_packet
    assert "d3137f9298b47474ea96dc694d44c5c026710039" in review_packet
    assert "Changed stable-header fingerprints | 19" in review_packet
    perf_review_packet = (
        repo_root / "docs" / "reviews" / "perf-r1-stable-core-additive-review.md"
    ).read_text(encoding="utf-8")
    assert "api-r1-perf-r1-reviewed-surface" in perf_review_packet
    assert "6b292156e3e94d3389e9f3b8513445e7eb4ab541" in perf_review_packet
    assert "TcpConnection::setSendBufferSize" in perf_review_packet
    assert "approved-additive-source-compatible" in perf_review_packet
    assert comparer.render_diff(difference) == comparer.render_diff(
        comparer.build_diff(
            baseline,
            historical_candidate,
            baseline_path=baseline_reference["path"],
            candidate_path="api/public_api_manifest.json",
        )
    )

    with tempfile.TemporaryDirectory(prefix="gamenet-api-manifest-") as directory:
        temporary = Path(directory) / "manifest.json"

        missing_header = copy.deepcopy(manifest)
        removed = missing_header["headers"]["stable_core"].pop()
        missing_header["stable_header_fingerprints"].pop(removed)
        temporary.write_text(json.dumps(missing_header), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("public header inventory mismatch" in error for error in errors)

        modified_surface = copy.deepcopy(manifest)
        stable_header = modified_surface["headers"]["stable_core"][0]
        modified_surface["stable_header_fingerprints"][stable_header] = "0" * 64
        temporary.write_text(json.dumps(modified_surface), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("stable public surface changed" in error for error in errors)

        wrong_version = copy.deepcopy(manifest)
        wrong_version["package_version"] = "99.0.0"
        temporary.write_text(json.dumps(wrong_version), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("package_version must match CMake" in error for error in errors)

        wrong_line = copy.deepcopy(manifest)
        wrong_line["compatibility_line"] = "0.2"
        temporary.write_text(json.dumps(wrong_line), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("compatibility_line must match CMake" in error for error in errors)

        wrong_release = copy.deepcopy(manifest)
        wrong_release["release_label"] = "v9.9.9-production-candidate"
        temporary.write_text(json.dumps(wrong_release), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("release_label version must match CMake" in error for error in errors)

        unclassified_target = copy.deepcopy(manifest)
        unclassified_target["targets"]["provisional"].pop()
        temporary.write_text(json.dumps(unclassified_target), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("exported target inventory mismatch" in error for error in errors)

        overlapping_target = copy.deepcopy(manifest)
        overlapping_target["targets"]["provisional"].append("GameNet::core")
        temporary.write_text(json.dumps(overlapping_target), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("targets categories overlap" in error for error in errors)

        wrong_baseline_tag = copy.deepcopy(manifest)
        wrong_baseline_tag["historical_baseline"]["tag"] = "v0.2.1-phase4-preview"
        temporary.write_text(json.dumps(wrong_baseline_tag), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("historical baseline tag does not match" in error for error in errors)

        wrong_baseline_hash = copy.deepcopy(manifest)
        wrong_baseline_hash["historical_baseline"]["snapshot_sha256"] = "0" * 64
        temporary.write_text(json.dumps(wrong_baseline_hash), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("snapshot hash mismatch" in error for error in errors)

        cli_output = Path(directory) / "public-api-diff.json"
        cli_compatibility_output = Path(directory) / "public-api-compatibility-diff.json"
        completed = subprocess.run(
            [
                sys.executable,
                str(repo_root / "tools" / "compare_public_api_manifest.py"),
                "--repo-root",
                str(repo_root),
                "--compatibility-baseline",
                "api/baselines/v0.3.0-perf-r1-reviewed.json",
                "--fail-on-compatibility-decision",
                "--fail-on-stable-surface-review",
                "--compatibility-output",
                str(cli_compatibility_output),
                "--output",
                str(cli_output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        assert completed.returncode == 0, completed.stderr
        assert json.loads(cli_output.read_text(encoding="utf-8")) == current_difference
        assert (
            json.loads(cli_compatibility_output.read_text(encoding="utf-8"))
            == reviewed_difference
        )

        for addition_kind in ("header", "target"):
            addition_root = Path(directory) / f"same-line-{addition_kind}-addition"
            addition_historical = addition_root / baseline_reference["path"]
            addition_historical.parent.mkdir(parents=True)
            addition_historical.write_text(json.dumps(baseline), encoding="utf-8")
            addition_reviewed = (
                addition_root
                / "api"
                / "baselines"
                / "v0.3.0-perf-r1-reviewed.json"
            )
            addition_reviewed.parent.mkdir(parents=True, exist_ok=True)
            addition_reviewed.write_text(
                json.dumps(reviewed_snapshot), encoding="utf-8"
            )
            addition_candidate = copy.deepcopy(manifest)
            if addition_kind == "header":
                added_header = "include/gamenet/core/net/ReviewedDrift.h"
                addition_candidate["headers"]["stable_core"].append(added_header)
                addition_candidate["headers"]["stable_core"].sort()
                addition_candidate["stable_header_fingerprints"][added_header] = "0" * 64
                addition_candidate["stable_header_fingerprints"] = dict(
                    sorted(addition_candidate["stable_header_fingerprints"].items())
                )
            else:
                addition_candidate["targets"]["stable_core"].append(
                    "GameNet::reviewed_drift"
                )
                addition_candidate["targets"]["stable_core"].sort()
            addition_manifest = addition_root / "api" / "public_api_manifest.json"
            addition_manifest.write_text(
                json.dumps(addition_candidate), encoding="utf-8"
            )
            addition_output = addition_root / "compatibility-diff.json"
            addition_rejected = subprocess.run(
                [
                    sys.executable,
                    str(repo_root / "tools" / "compare_public_api_manifest.py"),
                    "--repo-root",
                    str(addition_root),
                    "--compatibility-baseline",
                    "api/baselines/v0.3.0-perf-r1-reviewed.json",
                    "--fail-on-stable-surface-review",
                    "--compatibility-output",
                    str(addition_output),
                    "--output",
                    str(addition_root / "historical-diff.json"),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            assert addition_rejected.returncode == 3
            assert "requires stable-surface review" in addition_rejected.stderr
            addition_difference = json.loads(
                addition_output.read_text(encoding="utf-8")
            )
            assert addition_difference["summary"]["same_compatibility_line"] is True
            assert addition_difference["summary"]["stable_surface_review_required"] is True

        synthetic_root = Path(directory) / "same-line"
        synthetic_baseline = synthetic_root / baseline_reference["path"]
        synthetic_baseline.parent.mkdir(parents=True)
        synthetic_baseline.write_text(json.dumps(baseline), encoding="utf-8")
        synthetic_candidate = candidate_from_snapshot(baseline, baseline_reference)
        synthetic_stable = synthetic_candidate["headers"]["stable_core"][0]
        synthetic_candidate["stable_header_fingerprints"][synthetic_stable] = "0" * 64
        synthetic_manifest = synthetic_root / "api" / "public_api_manifest.json"
        synthetic_manifest.write_text(json.dumps(synthetic_candidate), encoding="utf-8")
        rejected = subprocess.run(
            [
                sys.executable,
                str(repo_root / "tools" / "compare_public_api_manifest.py"),
                "--repo-root",
                str(synthetic_root),
                "--fail-on-compatibility-decision",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        assert rejected.returncode == 2
        assert "requires a compatibility decision" in rejected.stderr

    same_line = candidate_from_snapshot(baseline, baseline_reference)
    stable_header = same_line["headers"]["stable_core"][0]
    same_line["stable_header_fingerprints"][stable_header] = "0" * 64
    same_line_diff = comparer.build_diff(
        baseline,
        same_line,
        baseline_path=baseline_reference["path"],
        candidate_path="api/public_api_manifest.json",
    )
    assert same_line_diff["summary"]["same_compatibility_line"] is True
    assert same_line_diff["summary"]["stable_surface_review_required"] is True
    assert same_line_diff["summary"]["compatibility_decision_required"] is True
    assert same_line_diff["summary"]["compatibility_decision_reasons"] == [
        f"stable header fingerprint changed: {stable_header}"
    ]

    classified_changes = candidate_from_snapshot(baseline, baseline_reference)
    classified_changes["targets"]["provisional"].remove("GameNet::transport")
    classified_changes["targets"]["provisional"].append("GameNet::new_preview")
    classified_changes["targets"]["provisional"].sort()
    classified_changes["targets"]["stable_core"].remove("GameNet::core")
    classified_changes["targets"]["provisional"].append("GameNet::core")
    classified_changes["targets"]["provisional"].sort()
    removed_header = "include/gamenet/transport/TransportEndpoint.h"
    classified_changes["headers"]["provisional"].remove(removed_header)
    classified_changes["headers"]["provisional"].append(
        "include/gamenet/transport/NewPreviewEndpoint.h"
    )
    moved_header = "include/gamenet/core/base/Logger.h"
    classified_changes["headers"]["stable_core"].remove(moved_header)
    classified_changes["headers"]["provisional"].append(moved_header)
    classified_changes["headers"]["provisional"].sort()
    classified_changes["stable_header_fingerprints"].pop(moved_header)
    changed_header = "include/gamenet/core/base/Timestamp.h"
    classified_changes["stable_header_fingerprints"][changed_header] = "f" * 64
    classified_diff = comparer.build_diff(
        baseline,
        classified_changes,
        baseline_path=baseline_reference["path"],
        candidate_path="api/public_api_manifest.json",
    )
    assert classified_diff["changes"]["targets"] == {
        "added": [{"name": "GameNet::new_preview", "category": "provisional"}],
        "removed": [{"name": "GameNet::transport", "category": "provisional"}],
        "category_moves": [
            {"name": "GameNet::core", "from": "stable_core", "to": "provisional"}
        ],
    }
    assert {
        "name": removed_header,
        "category": "provisional",
    } in classified_diff["changes"]["headers"]["removed"]
    assert {
        "name": "include/gamenet/transport/NewPreviewEndpoint.h",
        "category": "provisional",
    } in classified_diff["changes"]["headers"]["added"]
    assert {
        "name": moved_header,
        "from": "stable_core",
        "to": "provisional",
    } in classified_diff["changes"]["headers"]["category_moves"]
    assert classified_diff["changes"]["stable_header_fingerprint_changes"] == [
        {
            "header": changed_header,
            "before": baseline["stable_header_fingerprints"][changed_header],
            "after": "f" * 64,
        }
    ]
    assert classified_diff["summary"]["compatibility_decision_required"] is True

    provisional_only = candidate_from_snapshot(baseline, baseline_reference)
    provisional_only["targets"]["provisional"].append("GameNet::preview_only")
    provisional_only["targets"]["provisional"].sort()
    provisional_only_diff = comparer.build_diff(
        baseline,
        provisional_only,
        baseline_path=baseline_reference["path"],
        candidate_path="api/public_api_manifest.json",
    )
    assert provisional_only_diff["summary"]["has_changes"] is True
    assert provisional_only_diff["summary"]["stable_surface_review_required"] is False
    assert provisional_only_diff["summary"]["compatibility_decision_required"] is False

    normalized = verifier.normalize_cpp_public_surface(
        'int value; // comment\nconst char* url = "https://example.invalid/a"; /* block */\n'
    )
    assert normalized == 'int value; const char* url = "https://example.invalid/a";'

    print("public API manifest v2 and structured diff contracts verified")


if __name__ == "__main__":
    main()
