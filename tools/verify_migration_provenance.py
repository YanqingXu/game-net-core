from __future__ import annotations

import argparse
import subprocess
from pathlib import Path, PurePosixPath


SOURCE_REPOSITORY = "YanqingXu/mini_trantor"
SOURCE_COMMIT = "3eba368475a68f677aae920d4f299b155db23d57"
PHASE4_INTENTS = (
    "intents/modules/packet_framer.intent.md",
    "intents/modules/transport_endpoint.intent.md",
    "intents/modules/player_session.intent.md",
    "intents/modules/logic_loop.intent.md",
    "intents/modules/broadcast.intent.md",
    "intents/usecases/game_server_pipeline_demo.intent.md",
    "intents/usecases/phase4_performance_baseline.intent.md",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_front_matter(path: Path) -> dict[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    require(lines and lines[0] == "---", f"missing front matter: {path}")
    metadata: dict[str, str] = {}
    for line in lines[1:]:
        if line == "---":
            return metadata
        key, separator, value = line.partition(":")
        require(bool(separator), f"invalid front matter line in {path}: {line}")
        metadata[key.strip()] = value.strip()
    raise AssertionError(f"unterminated front matter: {path}")


def run_git(source_root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(source_root), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    require(
        result.returncode == 0,
        f"git {' '.join(arguments)} failed in {source_root}: {result.stderr.strip()}",
    )
    return result.stdout.strip()


def validate_source_path(source_path: str, intent_path: Path) -> str:
    require(bool(source_path), f"empty source path in {intent_path}")
    require("\\" not in source_path, f"source path must use '/' in {intent_path}: {source_path}")
    parsed = PurePosixPath(source_path)
    require(
        not parsed.is_absolute() and ".." not in parsed.parts and "." not in parsed.parts,
        f"source path must stay repository-relative in {intent_path}: {source_path}",
    )
    return parsed.as_posix()


def verify(repo_root: Path, source_root: Path) -> tuple[int, int]:
    require(source_root.is_dir(), f"missing migration source checkout: {source_root}")
    head = run_git(source_root, "rev-parse", "--verify", "HEAD")
    require(
        head == SOURCE_COMMIT,
        f"{SOURCE_REPOSITORY} HEAD mismatch: expected {SOURCE_COMMIT}, got {head}",
    )
    require(
        run_git(source_root, "cat-file", "-t", SOURCE_COMMIT) == "commit",
        f"migration source object is not a commit: {SOURCE_COMMIT}",
    )

    verified_paths = 0
    for relative_intent in PHASE4_INTENTS:
        intent_path = repo_root / relative_intent
        require(intent_path.is_file(), f"missing Phase 4 intent: {relative_intent}")
        metadata = parse_front_matter(intent_path)
        require(metadata.get("status") == "active", f"Phase 4 intent is not active: {relative_intent}")
        require(
            metadata.get("migration_source") == "mini_trantor",
            f"Phase 4 intent has the wrong migration source: {relative_intent}",
        )
        require(
            metadata.get("source_commit") == SOURCE_COMMIT,
            f"Phase 4 intent source commit drifted: {relative_intent}",
        )
        source_paths = metadata.get("source_paths", "").split(";")
        require(source_paths != [""], f"Phase 4 intent has no source paths: {relative_intent}")
        for declared_path in source_paths:
            source_path = validate_source_path(declared_path.strip(), intent_path)
            object_name = f"{SOURCE_COMMIT}:{source_path}"
            require(
                run_git(source_root, "cat-file", "-t", object_name) == "blob",
                f"migration source path is not a file at {SOURCE_COMMIT}: {relative_intent}: {source_path}",
            )
            verified_paths += 1

    return len(PHASE4_INTENTS), verified_paths


def main() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Verify Phase 4 intent paths against the immutable mini_trantor source commit."
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=repo_root / "mini_trantor",
        help="Path to the mini_trantor Git checkout (default: <repo>/mini_trantor).",
    )
    arguments = parser.parse_args()
    intent_count, source_path_count = verify(repo_root, arguments.source_root.resolve())
    print(
        f"verified {intent_count} Phase 4 intents and {source_path_count} source paths "
        f"against {SOURCE_REPOSITORY}@{SOURCE_COMMIT}"
    )


if __name__ == "__main__":
    main()
