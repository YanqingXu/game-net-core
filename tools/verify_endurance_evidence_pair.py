from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

from verify_endurance_evidence import EvidenceError, load_and_verify


SCHEMA = "gamenet.production_endurance_pair.v1"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify same-commit 24/72-hour endurance evidence")
    parser.add_argument("--candidate-evidence", type=Path, required=True)
    parser.add_argument("--release-evidence", type=Path, required=True)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    try:
        candidate = load_and_verify(
            arguments.candidate_evidence,
            "candidate-24h",
            arguments.candidate_sha,
            arguments.platform,
            arguments.backend,
        )
        release = load_and_verify(
            arguments.release_evidence,
            "release-72h",
            arguments.candidate_sha,
            arguments.platform,
            arguments.backend,
        )
        if arguments.output.exists():
            raise EvidenceError("pair output already exists")
        document = {
            "schema": SCHEMA,
            "generated_at_utc": datetime.now(timezone.utc)
            .isoformat(timespec="seconds")
            .replace("+00:00", "Z"),
            "status": "success",
            "candidate_sha": arguments.candidate_sha,
            "platform": arguments.platform,
            "backend": arguments.backend,
            "candidate_24h": {
                "file": arguments.candidate_evidence.name,
                "sha256": sha256_file(arguments.candidate_evidence),
                "completed_cycles": candidate["completed_cycles"],
                "elapsed_milliseconds": candidate["child_elapsed_milliseconds"],
            },
            "release_72h": {
                "file": arguments.release_evidence.name,
                "sha256": sha256_file(arguments.release_evidence),
                "completed_cycles": release["completed_cycles"],
                "elapsed_milliseconds": release["child_elapsed_milliseconds"],
            },
        }
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    except (EvidenceError, OSError) as error:
        print(f"endurance evidence pair verification failed: {error}", file=sys.stderr)
        return 1

    print(f"verified same-commit 24/72-hour endurance pair: {arguments.candidate_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
