# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
from pathlib import Path


CORPUS = {
    "valid-empty-frame": b"\x00\x00\x00\x00",
    "valid-one-byte-frame": b"\x00\x00\x00\x01A",
    "partial-prefix": b"\x00\x00\x00",
    "oversized-length": b"\xff\xff\xff\xff",
    "sticky-valid-frames": b"\x00\x00\x00\x01A\x00\x00\x00\x02BC",
    "valid-64-byte-frame": b"\x00\x00\x00\x40" + b"x" * 64,
}

HP1_CORPUS = {
    **CORPUS,
    "empty-frame-budget": b"\x00" * 40,
    "valid-before-oversized": b"\x00\x00\x00\x01A\x00\x00\x00\x41",
    "partial-max-payload": b"\x00\x00\x00\x40" + b"x" * 63,
    "buffer-limit": b"\x00" * 69,
    "zero-payload-limit": b"\x01\x00\x00\x00",
    "maximum-input": b"\xff" * 8192,
}


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate deterministic PacketFramer fuzz seeds")
    parser.add_argument("--check", action="store_true", help="verify seeds without rewriting them")
    parser.add_argument("--hp1", action="store_true", help="select the HP1 differential corpus")
    args = parser.parse_args()

    corpus = HP1_CORPUS if args.hp1 else CORPUS
    corpus_name = "hp1_packet_framer_view" if args.hp1 else "packet_framer"
    corpus_dir = Path(__file__).resolve().parent / "corpus" / corpus_name
    corpus_dir.mkdir(parents=True, exist_ok=True)
    for name, expected in corpus.items():
        path = corpus_dir / name
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f"PacketFramer corpus seed is missing or stale: {path}")
        else:
            path.write_bytes(expected)

    unexpected = sorted(path.name for path in corpus_dir.iterdir() if path.name not in corpus)
    if unexpected:
        raise SystemExit(f"unexpected PacketFramer corpus entries: {', '.join(unexpected)}")


if __name__ == "__main__":
    main()
