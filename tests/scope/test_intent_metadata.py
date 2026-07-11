from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


CATALOG_HEADINGS = {
    "## Active Intents": "active",
    "## Deferred Intent Catalog": "deferred",
    "## Legacy Intent Catalog": "legacy",
}
BASE_METADATA_KEYS = ("status", "target", "migration_source", "promote_gate")
ENRICHED_METADATA_KEYS = (
    "artifact_kind",
    "migration_mode",
    "source_commit",
    "source_paths",
)
ALLOWED_STATUSES = {"active", "deferred", "legacy"}
ALLOWED_TARGETS = {
    "GameNet::core",
    "GameNet::protocol",
    "GameNet::transport",
    "GameNet::game_session",
    "GameNet::game_logic",
    "GameNet::broadcast",
    "GameNet::game",
    "GameNet::experimental",
    "gamenet_echo_server",
    "gamenet_core_benchmark",
    "gamenet_game_server_pipeline_demo",
    "gamenet_phase4_benchmark",
    "historical",
}
ALLOWED_SOURCES = {"mini_trantor", "native"}
ALLOWED_ARTIFACT_KINDS = {"installed-library", "example", "benchmark"}
ALLOWED_MIGRATION_MODES = {"adapt", "redesign", "native"}
ALLOWED_GATES = {
    "none",
    "phase-4-protocol",
    "phase-4-transport",
    "phase-4-game-foundation",
    "post-core-preview",
    "post-phase-4-protocol",
    "experimental-only",
    "never",
}


@dataclass(frozen=True)
class IntentMetadata:
    status: str
    target: str
    migration_source: str
    promote_gate: str
    artifact_kind: str | None = None
    migration_mode: str | None = None
    source_commit: str | None = None
    source_paths: str | None = None


PHASE4_ACTIVE_INTENTS = {
    "intents/modules/packet_framer.intent.md",
    "intents/modules/transport_endpoint.intent.md",
    "intents/modules/player_session.intent.md",
    "intents/modules/logic_loop.intent.md",
    "intents/usecases/game_server_pipeline_demo.intent.md",
    "intents/usecases/phase4_performance_baseline.intent.md",
    "intents/modules/broadcast.intent.md",
}

CONCRETE_ACTIVE_ARTIFACTS = {
    "intents/usecases/echo_server.intent.md": ("example", "gamenet_echo_server"),
    "intents/usecases/core_performance_baseline.intent.md": (
        "benchmark",
        "gamenet_core_benchmark",
    ),
    "intents/modules/packet_framer.intent.md": ("installed-library", "GameNet::protocol"),
    "intents/modules/transport_endpoint.intent.md": ("installed-library", "GameNet::transport"),
    "intents/modules/player_session.intent.md": (
        "installed-library",
        "GameNet::game_session",
    ),
    "intents/modules/logic_loop.intent.md": ("installed-library", "GameNet::game_logic"),
    "intents/modules/broadcast.intent.md": ("installed-library", "GameNet::broadcast"),
    "intents/usecases/game_server_pipeline_demo.intent.md": (
        "example",
        "gamenet_game_server_pipeline_demo",
    ),
    "intents/usecases/phase4_performance_baseline.intent.md": (
        "benchmark",
        "gamenet_phase4_benchmark",
    ),
}

BACKPRESSURE_DERIVED_ACTIVE_INTENTS = {
    "intents/modules/logic_loop.intent.md",
    "intents/modules/broadcast.intent.md",
    "intents/modules/packet_framer.intent.md",
    "intents/usecases/game_server_pipeline_demo.intent.md",
}


def require(condition: bool, message: str) -> None:
    assert condition, message


def formal_intent_paths(repo_root: Path) -> set[str]:
    return {
        path.relative_to(repo_root).as_posix()
        for path in (repo_root / "intents").rglob("*.intent.md")
    }


def parse_metadata(path: Path) -> tuple[IntentMetadata, str]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    require(len(lines) >= 8, f"intent is too short for metadata and a heading: {path}")
    require(lines[0] == "---", f"intent metadata must start on line 1: {path}")

    try:
        metadata_end = lines.index("---", 1)
    except ValueError:
        require(False, f"intent metadata has no closing delimiter: {path}")
        raise AssertionError("unreachable")

    values: dict[str, str] = {}
    keys: list[str] = []
    for line in lines[1:metadata_end]:
        match = re.fullmatch(r"([a-z_]+): (\S+)", line)
        require(match is not None, f"invalid intent metadata line in {path}: {line!r}")
        key, value = match.groups()
        require(key not in values, f"duplicate metadata key in {path}: {key}")
        require(
            key in BASE_METADATA_KEYS + ENRICHED_METADATA_KEYS,
            f"unknown metadata key in {path}: {key}",
        )
        keys.append(key)
        values[key] = value

    require(
        tuple(keys[: len(BASE_METADATA_KEYS)]) == BASE_METADATA_KEYS,
        f"base metadata key order mismatch in {path}: expected {BASE_METADATA_KEYS}",
    )
    require(
        keys[len(BASE_METADATA_KEYS) :] == [
            key for key in ENRICHED_METADATA_KEYS if key in values
        ],
        f"enriched metadata key order mismatch in {path}: expected {ENRICHED_METADATA_KEYS}",
    )
    require(
        metadata_end + 2 < len(lines) and lines[metadata_end + 1] == "",
        f"intent metadata must be followed by one blank line: {path}",
    )
    require(lines[metadata_end + 2].startswith("# "), f"intent heading must follow metadata in {path}")
    metadata = IntentMetadata(**values)
    return metadata, "\n".join(lines[metadata_end + 2 :])


def parse_catalogs(index: Path) -> dict[str, str]:
    catalog: dict[str, str] = {}
    current_status: str | None = None
    for line in index.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped in CATALOG_HEADINGS:
            current_status = CATALOG_HEADINGS[stripped]
            continue
        if stripped.startswith("## "):
            current_status = None
            continue
        if current_status is None:
            continue
        match = re.fullmatch(r"- `([^`]+\.intent\.md)`", stripped)
        if match is None:
            continue
        relative_path = match.group(1)
        require(relative_path not in catalog, f"intent appears in multiple catalogs: {relative_path}")
        catalog[relative_path] = current_status
    return catalog


def validate_status_contract(path: str, metadata: IntentMetadata) -> None:
    require(metadata.status in ALLOWED_STATUSES, f"invalid status in {path}: {metadata.status}")
    require(metadata.target in ALLOWED_TARGETS, f"invalid target in {path}: {metadata.target}")
    require(
        metadata.migration_source in ALLOWED_SOURCES,
        f"invalid migration_source in {path}: {metadata.migration_source}",
    )
    require(metadata.promote_gate in ALLOWED_GATES, f"invalid promote_gate in {path}: {metadata.promote_gate}")

    if metadata.status == "active":
        require(metadata.target != "historical", f"active intent cannot target historical: {path}")
        require(metadata.promote_gate == "none", f"active intent must use promote_gate none: {path}")
    elif metadata.status == "deferred":
        require(metadata.target != "historical", f"deferred intent cannot target historical: {path}")
        require(
            metadata.promote_gate not in {"none", "never"},
            f"deferred intent needs a future promotion gate: {path}",
        )
    else:
        require(metadata.target == "historical", f"legacy intent must target historical: {path}")
        require(metadata.promote_gate == "never", f"legacy intent must use promote_gate never: {path}")

    target_gate_pairs = {
        "phase-4-transport": "GameNet::transport",
        "phase-4-game-foundation": "GameNet::game",
        "post-phase-4-protocol": "GameNet::protocol",
        "experimental-only": "GameNet::experimental",
    }
    expected_target = target_gate_pairs.get(metadata.promote_gate)
    if expected_target is not None:
        require(metadata.target == expected_target, f"gate/target mismatch in {path}")

    concrete_artifact = CONCRETE_ACTIVE_ARTIFACTS.get(path)
    if concrete_artifact is not None:
        expected_kind, expected_artifact_target = concrete_artifact
        require(metadata.status == "active", f"implemented artifact must be active: {path}")
        require(
            metadata.artifact_kind == expected_kind,
            f"artifact kind mismatch in {path}: expected {expected_kind}",
        )
        require(
            metadata.target == expected_artifact_target,
            f"artifact target mismatch in {path}: expected {expected_artifact_target}",
        )

    if path in PHASE4_ACTIVE_INTENTS:
        require(metadata.status == "active", f"Phase 4 implemented intent must be active: {path}")
        require(
            metadata.artifact_kind in ALLOWED_ARTIFACT_KINDS,
            f"Phase 4 active intent needs artifact_kind in {path}",
        )
        require(
            metadata.migration_source == "mini_trantor",
            f"Phase 4 redesigned intent must preserve mini_trantor provenance: {path}",
        )
        require(
            metadata.migration_mode in ALLOWED_MIGRATION_MODES,
            f"Phase 4 active intent needs migration_mode in {path}",
        )
        require(
            metadata.source_commit is not None
            and re.fullmatch(r"[0-9a-f]{40}", metadata.source_commit) is not None,
            f"Phase 4 active intent needs a 40-character source_commit in {path}",
        )
        require(
            metadata.source_paths not in {None, "none"},
            f"Phase 4 active intent needs source_paths in {path}",
        )


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    index = repo_root / "intents" / "README.md"
    formal_paths = formal_intent_paths(repo_root)
    catalog = parse_catalogs(index)

    require(formal_paths == set(catalog), (
        "intent catalogs must cover every formal intent exactly once; "
        f"missing={sorted(formal_paths - set(catalog))}, extra={sorted(set(catalog) - formal_paths)}"
    ))

    counts = {status: 0 for status in ALLOWED_STATUSES}
    for relative_path in sorted(formal_paths):
        metadata, body = parse_metadata(repo_root / relative_path)
        validate_status_contract(relative_path, metadata)
        require(
            catalog[relative_path] == metadata.status,
            f"catalog/front-matter status mismatch for {relative_path}",
        )
        counts[metadata.status] += 1

        if metadata.status == "active":
            for stale_fragment in ("MINI_ENABLE_", "mini::", "mini/net", "mini-trantor"):
                require(
                    stale_fragment not in body,
                    f"active intent contains source-project contract {stale_fragment!r}: {relative_path}",
                )
        if relative_path in PHASE4_ACTIVE_INTENTS:
            require("## Migration Provenance" in body, f"missing migration provenance: {relative_path}")
            for label in ("Kept invariants:", "Deferred", "Dropped"):
                require(label in body, f"missing {label!r} provenance note: {relative_path}")

    require(all(counts.values()), f"every intent status must have at least one document: {counts}")

    backpressure_path = "intents/modules/game_backpressure_policy.intent.md"
    backpressure_metadata, backpressure_body = parse_metadata(repo_root / backpressure_path)
    require(
        backpressure_metadata.status == "deferred",
        "the end-to-end game backpressure policy must remain deferred",
    )
    require(
        "## Partial Supersession Record" in backpressure_body,
        "game backpressure must record its partial active supersession",
    )
    require(
        "Implementations must not cite this deferred document as authority." in backpressure_body,
        "game backpressure must deny implementation authority to its deferred remainder",
    )
    partial_record = backpressure_body.split("## 1. Intent", maxsplit=1)[0]
    derived_references = set(
        re.findall(r"`(intents/[^`]+\.intent\.md)`", partial_record)
    )
    require(
        derived_references == BACKPRESSURE_DERIVED_ACTIVE_INTENTS,
        "game backpressure active supersession references drifted: "
        f"missing={sorted(BACKPRESSURE_DERIVED_ACTIVE_INTENTS - derived_references)}, "
        f"extra={sorted(derived_references - BACKPRESSURE_DERIVED_ACTIVE_INTENTS)}",
    )
    for derived_path in sorted(derived_references):
        derived_metadata, _ = parse_metadata(repo_root / derived_path)
        require(
            derived_metadata.status == "active",
            f"game backpressure derives authority from a non-active intent: {derived_path}",
        )

    echo_path = "intents/usecases/echo_server.intent.md"
    echo_metadata, echo_body = parse_metadata(repo_root / echo_path)
    require(echo_metadata.status == "active", "implemented echo use case must be active")
    require("GameNet::core" in echo_body, "echo intent must name the migrated target")
    require(
        "tests/integration/tcp/test_tcp_server_client_echo.cpp" in echo_body,
        "echo intent must name its integration contract",
    )

    review_rules = repo_root / "rules" / "review_rules.md"
    review_text = review_rules.read_text(encoding="utf-8")
    require(
        "intent reference whose metadata status is `active`" in review_text,
        "review rules must require active intent metadata",
    )
    require(
        "A `deferred` or `legacy` intent cannot authorize implementation" in review_text,
        "review rules must reject deferred/legacy implementation authority",
    )
    require(
        "v1-coro-preview" not in review_text,
        "current review rules must not route work through a legacy source-project stage",
    )

    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    long_soak = repo_root / ".github" / "workflows" / "long-soak.yml"
    workflow_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    long_soak_contract = repo_root / "tests" / "ci" / "test_long_soak_workflow.py"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    linux_guard = "python3 tests/scope/test_intent_metadata.py"
    windows_guard = "python tests/scope/test_intent_metadata.py"
    linux_semantic_guard = "python3 tests/scope/test_intent_semantics.py"
    windows_semantic_guard = "python tests/scope/test_intent_semantics.py"
    require(linux_guard in workflow.read_text(encoding="utf-8"), "Linux CI must run intent metadata guard")
    require(windows_guard in workflow.read_text(encoding="utf-8"), "Windows CI must run intent metadata guard")
    require(linux_semantic_guard in workflow.read_text(encoding="utf-8"), "Linux CI must run intent semantic guard")
    require(windows_semantic_guard in workflow.read_text(encoding="utf-8"), "Windows CI must run intent semantic guard")
    require(linux_guard in long_soak.read_text(encoding="utf-8"), "long-soak must run intent metadata guard")
    require(linux_semantic_guard in long_soak.read_text(encoding="utf-8"), "long-soak must run intent semantic guard")
    require(
        linux_guard in workflow_contract.read_text(encoding="utf-8"),
        "workflow contract must require intent metadata guard",
    )
    require(
        linux_guard in long_soak_contract.read_text(encoding="utf-8"),
        "long-soak contract must require intent metadata guard",
    )
    require(
        linux_semantic_guard in workflow_contract.read_text(encoding="utf-8"),
        "workflow contract must require intent semantic guard",
    )
    require(
        linux_semantic_guard in long_soak_contract.read_text(encoding="utf-8"),
        "long-soak contract must require intent semantic guard",
    )
    require(
        "Intent metadata contract guard" in ci_docs.read_text(encoding="utf-8"),
        "CI docs must describe intent metadata guard",
    )
    require(
        "Intent target/test semantic guard" in ci_docs.read_text(encoding="utf-8"),
        "CI docs must describe intent semantic guard",
    )


if __name__ == "__main__":
    main()
