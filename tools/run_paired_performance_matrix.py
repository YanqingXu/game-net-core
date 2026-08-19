#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_performance_matrix import (  # noqa: E402
    SCHEMA,
    SCENARIO_PROFILES,
    MatrixError,
    Scenario,
    prepare_empty_directory,
    require,
    sha256_file,
    validate_document,
)


SAMPLING_SCHEMA = "gamenet.paired_interleaved.v1"


@dataclass
class Side:
    name: str
    commit_sha: str
    executables: dict[str, Path]
    output_root: Path
    canonical_core_dir: Path | None = None
    canonical_phase4_dir: Path | None = None
    allow_legacy_core_v1: bool = False
    scenario_records: list[dict[str, Any]] = field(default_factory=list)

    def canonical_root(self, group: str) -> Path | None:
        return (
            self.canonical_core_dir
            if group == "core"
            else self.canonical_phase4_dir
        )


def resolve_executables(
    scenarios: tuple[Scenario, ...],
    core: Path,
    phase4: Path | None,
    label: str,
) -> dict[str, Path]:
    arguments = {"core": core, "phase4": phase4}
    executables: dict[str, Path] = {}
    for group in {scenario.group for scenario in scenarios}:
        executable = arguments[group]
        require(executable is not None, f"{label} {group} executable argument missing")
        resolved = executable.resolve()
        require(resolved.is_file(), f"{label} {group} executable missing: {resolved}")
        executables[group] = resolved
    return executables


def run_sample(
    side: Side,
    scenario: Scenario,
    arguments: argparse.Namespace,
    label: str,
) -> dict[str, Any]:
    command = [str(side.executables[scenario.group]), *scenario.arguments]
    completed = subprocess.run(
        command,
        capture_output=True,
        timeout=arguments.process_timeout_seconds,
        check=False,
    )
    stderr = (
        completed.stderr
        if isinstance(completed.stderr, str)
        else completed.stderr.decode("utf-8", errors="backslashreplace")
    )
    require(
        completed.returncode == 0,
        f"{label} failed: {stderr.strip() or '<empty>'}",
    )
    try:
        stdout = (
            completed.stdout
            if isinstance(completed.stdout, str)
            else completed.stdout.decode("utf-8", errors="strict")
        )
    except UnicodeDecodeError as error:
        raise MatrixError(f"{label} stdout is not UTF-8: {error}") from error
    try:
        document = json.loads(stdout.lstrip("\ufeff"))
    except json.JSONDecodeError as error:
        raise MatrixError(f"{label} emitted invalid JSON: {error}") from error
    return validate_document(
        document,
        scenario,
        arguments.platform,
        arguments.backend,
        arguments.build_type,
        allow_legacy_core_v1=side.allow_legacy_core_v1,
    )


def prepare_side(side: Side, scenarios: tuple[Scenario, ...]) -> None:
    prepare_empty_directory(side.output_root, f"{side.name} performance matrix output")
    for group in {scenario.group for scenario in scenarios}:
        (side.output_root / group).mkdir()


def record_sample(
    side: Side,
    scenario: Scenario,
    repetition: int,
    document: dict[str, Any],
    samples: list[dict[str, str]],
) -> None:
    relative = Path(scenario.group) / f"{scenario.key}-{repetition}.json"
    output = side.output_root / relative
    output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    samples.append({"path": relative.as_posix(), "sha256": sha256_file(output)})

    if repetition == 1 and scenario.canonical:
        canonical_root = side.canonical_root(scenario.group)
        if canonical_root is not None:
            canonical_root.mkdir(parents=True, exist_ok=True)
            canonical = canonical_root / f"{scenario.key}.json"
            require(
                not canonical.exists(),
                f"canonical benchmark output already exists: {canonical}",
            )
            shutil.copy2(output, canonical)


def build_manifest(
    side: Side,
    peer: Side,
    arguments: argparse.Namespace,
) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "profile": arguments.matrix_profile,
        "commit_sha": side.commit_sha,
        "platform": arguments.platform,
        "backend": arguments.backend,
        "build_type": arguments.build_type,
        "repetitions": arguments.repetitions,
        "sampling": {
            "schema": SAMPLING_SCHEMA,
            "pair_role": side.name,
            "peer_commit_sha": peer.commit_sha,
            "warmups_per_scenario": 1,
            "order_rule": "scenario-and-repetition-parity",
        },
        "executables": {
            group: {"path": str(path), "sha256": sha256_file(path)}
            for group, path in sorted(side.executables.items())
        },
        "scenarios": side.scenario_records,
    }


def run_paired_matrix(arguments: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    require(
        arguments.repetitions >= 3 and arguments.repetitions % 2 == 1,
        "repetitions must be an odd number of at least 3",
    )
    for label, commit_sha in (
        ("candidate", arguments.candidate_sha),
        ("baseline", arguments.baseline_sha),
    ):
        require(
            len(commit_sha) == 40
            and all(character in "0123456789abcdef" for character in commit_sha),
            f"{label} commit SHA must be 40 lowercase hex digits",
        )

    scenarios = SCENARIO_PROFILES[arguments.matrix_profile]
    candidate = Side(
        name="candidate",
        commit_sha=arguments.candidate_sha,
        executables=resolve_executables(
            scenarios,
            arguments.candidate_core_executable,
            arguments.candidate_phase4_executable,
            "candidate",
        ),
        output_root=arguments.candidate_output_root,
        canonical_core_dir=arguments.candidate_canonical_core_dir,
        canonical_phase4_dir=arguments.candidate_canonical_phase4_dir,
    )
    baseline = Side(
        name="baseline",
        commit_sha=arguments.baseline_sha,
        executables=resolve_executables(
            scenarios,
            arguments.baseline_core_executable,
            arguments.baseline_phase4_executable,
            "baseline",
        ),
        output_root=arguments.baseline_output_root,
        allow_legacy_core_v1=arguments.matrix_profile == "regression",
    )
    prepare_side(candidate, scenarios)
    prepare_side(baseline, scenarios)

    sides = {"candidate": candidate, "baseline": baseline}
    for scenario_index, scenario in enumerate(scenarios):
        warmup_documents: dict[str, dict[str, Any]] = {}
        warmup_order = (
            ("candidate", "baseline")
            if scenario_index % 2 == 0
            else ("baseline", "candidate")
        )
        for side_name in warmup_order:
            side = sides[side_name]
            warmup_documents[side_name] = run_sample(
                side,
                scenario,
                arguments,
                f"{side_name} {scenario.key} warmup",
            )

        recorded: dict[str, list[dict[str, str]]] = {
            "candidate": [],
            "baseline": [],
        }
        for repetition in range(1, arguments.repetitions + 1):
            candidate_first = (scenario_index + repetition) % 2 == 1
            order = (
                ("candidate", "baseline")
                if candidate_first
                else ("baseline", "candidate")
            )
            for side_name in order:
                side = sides[side_name]
                document = run_sample(
                    side,
                    scenario,
                    arguments,
                    f"{side_name} {scenario.key} repetition {repetition}",
                )
                warmup = warmup_documents[side_name]
                require(
                    document["parameters"] == warmup["parameters"],
                    f"{side_name} {scenario.key}: parameters changed after warmup",
                )
                require(
                    document["schema"] == warmup["schema"],
                    f"{side_name} {scenario.key}: schema changed after warmup",
                )
                record_sample(
                    side,
                    scenario,
                    repetition,
                    document,
                    recorded[side_name],
                )

        for side_name, side in sides.items():
            warmup = warmup_documents[side_name]
            side.scenario_records.append(
                {
                    "key": f"{scenario.group}.{scenario.key}",
                    "group": scenario.group,
                    "reported_scenario": scenario.reported_scenario,
                    "schema": warmup["schema"],
                    "parameters": warmup["parameters"],
                    "samples": recorded[side_name],
                }
            )

    candidate_manifest = build_manifest(candidate, baseline, arguments)
    baseline_manifest = build_manifest(baseline, candidate, arguments)
    for side, manifest in (
        (candidate, candidate_manifest),
        (baseline, baseline_manifest),
    ):
        (side.output_root / "matrix-manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n",
            encoding="utf-8",
        )
    return candidate_manifest, baseline_manifest


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run baseline and candidate GameNet matrices as warm paired, interleaved samples"
    )
    parser.add_argument("--candidate-core-executable", type=Path, required=True)
    parser.add_argument("--candidate-phase4-executable", type=Path)
    parser.add_argument("--baseline-core-executable", type=Path, required=True)
    parser.add_argument("--baseline-phase4-executable", type=Path)
    parser.add_argument("--candidate-output-root", type=Path, required=True)
    parser.add_argument("--baseline-output-root", type=Path, required=True)
    parser.add_argument("--candidate-canonical-core-dir", type=Path)
    parser.add_argument("--candidate-canonical-phase4-dir", type=Path)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--baseline-sha", required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument(
        "--matrix-profile",
        choices=tuple(SCENARIO_PROFILES),
        default="regression",
    )
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--process-timeout-seconds", type=int, default=90)
    arguments = parser.parse_args()
    try:
        candidate, _baseline = run_paired_matrix(arguments)
    except (MatrixError, OSError, subprocess.SubprocessError) as error:
        print(f"paired performance matrix failed: {error}", file=sys.stderr)
        return 1
    print(
        "paired performance matrix completed: "
        f"{candidate['profile']} "
        f"{len(candidate['scenarios'])} scenarios x {arguments.repetitions} pairs"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
