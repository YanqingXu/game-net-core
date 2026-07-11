from __future__ import annotations

import re
from pathlib import Path


ACTIVE_SUPPORT_INTENTS = {
    "intents/modules/platform_runtime.intent.md",
}

LEGACY_ACTIVE_INTENT_PATTERNS = (
    ("legacy mini public path", re.compile(r"\bmini/net\b")),
    ("legacy mini include", re.compile(r'#\s*include\s*[<"]mini/')),
    ("legacy mini namespace", re.compile(r"\bmini::|\bnamespace\s+mini\b")),
)


def require(condition: bool, message: str) -> None:
    assert condition, message


def active_intents_from_index(repo_root: Path) -> set[str]:
    index_path = repo_root / "intents" / "README.md"
    active: set[str] = set()
    in_active_section = False

    for line in index_path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped == "## Active Intents":
            in_active_section = True
            continue
        if in_active_section and stripped.startswith("## "):
            break
        if in_active_section:
            match = re.search(r"`([^`]+\.intent\.md)`", stripped)
            if match is not None:
                active.add(match.group(1))

    return active


def legacy_violations(repo_root: Path, intent_paths: set[str]) -> list[str]:
    violations: list[str] = []
    for relative_path in sorted(intent_paths):
        path = repo_root / relative_path
        require(path.exists(), f"active intent is missing: {relative_path}")
        lines = path.read_text(encoding="utf-8").splitlines()
        require(lines and lines[0] == "---", f"active intent metadata is missing: {relative_path}")
        try:
            metadata_end = lines.index("---", 1)
        except ValueError as error:
            raise AssertionError(f"active intent metadata is unterminated: {relative_path}") from error

        # Provenance metadata names preserved source paths by design. Legacy
        # public paths and namespaces remain forbidden in the active contract
        # body where they could be mistaken for current APIs.
        for line_number, line in enumerate(lines[metadata_end + 1 :], start=metadata_end + 2):
            for kind, pattern in LEGACY_ACTIVE_INTENT_PATTERNS:
                match = pattern.search(line)
                if match is not None:
                    violations.append(f"{relative_path}:{line_number}: {kind}: {match.group(0)}")
    return violations


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    active_intents = active_intents_from_index(repo_root)

    missing_support = sorted(ACTIVE_SUPPORT_INTENTS - active_intents)
    require(
        not missing_support,
        "support intents used by the active core must be listed under Active Intents: "
        + ", ".join(missing_support),
    )

    violations = legacy_violations(repo_root, active_intents)
    require(
        not violations,
        "active intents must use gamenet/core paths and namespaces:\n" + "\n".join(violations),
    )

    platform_intent = (repo_root / "intents" / "modules" / "platform_runtime.intent.md").read_text(
        encoding="utf-8"
    )
    readme_text = (repo_root / "README.md").read_text(encoding="utf-8")
    require(
        "- Logger" in readme_text,
        "README Current Scope must list Logger once Logger is built and contract-tested in core",
    )
    require(
        "gamenet/core/net/platform/SocketTypes.h" in platform_intent,
        "platform runtime intent must name the migrated platform socket type header",
    )
    require(
        "gamenet/core/net/SocketsOps.h" in platform_intent,
        "platform runtime intent must name the migrated public SocketsOps header",
    )
    require(
        "gamenet/core/net/poller/" in platform_intent,
        "platform runtime intent must name the migrated poller backend directory",
    )


if __name__ == "__main__":
    main()
