#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path


FORMAL_INTENT_PATTERN = "*.intent.md"
STATUSES = ("active", "deferred", "legacy")
CATALOG_HEADINGS = {
    "## Active Intents": "active",
    "## Deferred Intent Catalog": "deferred",
    "## Legacy Intent Catalog": "legacy",
}
VERIFICATION_PATH_PATTERN = re.compile(
    r"(?<![A-Za-z0-9_./-])"
    r"(tests/[A-Za-z0-9_./-]+\.(?:cpp|py))"
    r"(?![A-Za-z0-9_./-])"
)


class IntentInventoryError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise IntentInventoryError(message)


@dataclass(frozen=True)
class IntentDocument:
    relative_path: str
    status: str
    body: str


@dataclass(frozen=True)
class IntentInventory:
    formal: int
    active: int
    deferred: int
    legacy: int
    verification_paths: int

    def as_dict(self) -> dict[str, int]:
        return {
            "formal": self.formal,
            "active": self.active,
            "deferred": self.deferred,
            "legacy": self.legacy,
            "verification_paths": self.verification_paths,
        }


def parse_intent(path: Path, repo_root: Path) -> IntentDocument:
    lines = path.read_text(encoding="utf-8").splitlines()
    relative_path = path.relative_to(repo_root).as_posix()
    require(
        bool(lines) and lines[0] == "---",
        f"intent front matter is missing: {relative_path}",
    )
    try:
        metadata_end = lines.index("---", 1)
    except ValueError as error:
        raise IntentInventoryError(
            f"intent front matter is unterminated: {relative_path}"
        ) from error

    metadata: dict[str, str] = {}
    for line in lines[1:metadata_end]:
        match = re.fullmatch(r"([a-z_]+):\s*(.*?)\s*", line)
        require(match is not None, f"invalid intent metadata: {relative_path}: {line}")
        key, value = match.groups()
        require(key not in metadata, f"duplicate intent metadata: {relative_path}: {key}")
        metadata[key] = value

    status = metadata.get("status", "")
    require(status in STATUSES, f"invalid intent status: {relative_path}: {status!r}")
    return IntentDocument(
        relative_path=relative_path,
        status=status,
        body="\n".join(lines[metadata_end + 1 :]),
    )


def catalog_entries(index_path: Path) -> dict[str, list[str]]:
    entries = {status: [] for status in STATUSES}
    current_status: str | None = None
    for line in index_path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("## "):
            current_status = CATALOG_HEADINGS.get(stripped)
            continue
        if current_status is None:
            continue
        match = re.fullmatch(r"- `([^`]+\.intent\.md)`", stripped)
        if match is not None:
            entries[current_status].append(match.group(1))
    return entries


def verification_paths(body: str) -> set[str]:
    return set(VERIFICATION_PATH_PATTERN.findall(body))


def build_inventory(repo_root: Path) -> IntentInventory:
    repo_root = repo_root.resolve()
    documents = {
        document.relative_path: document
        for document in (
            parse_intent(path, repo_root)
            for path in sorted((repo_root / "intents").rglob(FORMAL_INTENT_PATTERN))
        )
    }
    require(documents, "formal intent inventory is empty")

    catalogs = catalog_entries(repo_root / "intents" / "README.md")
    catalog_paths = [
        relative_path
        for status in STATUSES
        for relative_path in catalogs[status]
    ]
    duplicates = sorted(
        {
            relative_path
            for relative_path in catalog_paths
            if catalog_paths.count(relative_path) > 1
        }
    )
    require(not duplicates, f"intent catalog contains duplicates: {duplicates}")

    formal_paths = set(documents)
    indexed_paths = set(catalog_paths)
    require(
        formal_paths == indexed_paths,
        "intent catalog/formal inventory mismatch: "
        f"catalog-only={sorted(indexed_paths - formal_paths)}, "
        f"formal-only={sorted(formal_paths - indexed_paths)}",
    )

    for status in STATUSES:
        for relative_path in catalogs[status]:
            require(
                documents[relative_path].status == status,
                "intent catalog/front-matter status mismatch: "
                f"{relative_path}: catalog={status}, "
                f"front_matter={documents[relative_path].status}",
            )

    counts = {
        status: sum(document.status == status for document in documents.values())
        for status in STATUSES
    }
    active_verification_paths = sum(
        len(verification_paths(document.body))
        for document in documents.values()
        if document.status == "active"
    )
    return IntentInventory(
        formal=len(documents),
        active=counts["active"],
        deferred=counts["deferred"],
        legacy=counts["legacy"],
        verification_paths=active_verification_paths,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Derive the formal intent inventory from metadata and the intent index"
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    arguments = parser.parse_args()
    try:
        inventory = build_inventory(arguments.repo_root)
    except IntentInventoryError as error:
        print(f"intent inventory error: {error}")
        return 1
    print(json.dumps(inventory.as_dict(), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
