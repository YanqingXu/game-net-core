# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Execute two bounded RPC fuzz targets and retain source/run/execution evidence."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

KINDS = {"codec": 4096, "channel": 512}

def verify_log(text: str, runs: int, seed: int) -> None:
    counts = re.findall(r"stat::number_of_executed_units:\s+(\d+)", text)
    if counts != [str(runs)] or f"Seed: {seed}\n" not in text:
        raise ValueError("fuzz evidence must contain the exact execution count and seed")
    if re.search(r"ERROR:|runtime error:|SUMMARY:.*Sanitizer|WARNING: ThreadSanitizer|DEADLYSIGNAL", text):
        raise ValueError("fuzz evidence contains a sanitizer/fuzzer failure")

def corpus(kind: str) -> dict[str, bytes]:
    if kind == "codec":
        def body(kind: int, request: int, method: int, timeout: int, payload: bytes = b"") -> bytes:
            return struct.pack(">4sBBHQII", b"GNRP", 1, kind, 0, request, method, timeout) + payload
        request = body(1, 1, 7, 60000, b"binary\0payload")
        return {"request": request, "response": body(2, 1, 0, 0), "error": body(3, 2, 0, 0, b"error"),
                "truncated": request[:23], "bad-kind": body(255, 1, 0, 0),
                "zero-id": body(1, 0, 1, 1), "bad-method": body(2, 1, 1, 0),
                "bad-version": request[:4] + b"\xff" + request[5:], "oversized": request + b"x" * 4096}
    def actions(*values: tuple[int, ...]) -> bytes:
        return bytes(x for value in values for x in (*value, *(0 for _ in range(4 - len(value)))) )
    return {
        "terminals": actions((0, 1), (0, 2), (2, 0), (4, 0), (1, 1), (12,), (0,), (10,)),
        "overload": actions(*[(0, n) for n in range(10)], (1, 0), (0,), (9,)),
        "continuation": actions((16,), (0,), (0,), (15, 0), (12,), (0,), (15, 1)),
        "transport": actions((7, 0), (0,), (7, 1), (8, 1), (0,), (8, 2), (0,), (8, 0), (0,), (8, 3), (0,)),
        "inbound": actions((5, 1), (5, 1), (5, 1), (6,), (6,), (5, 2), (5, 0), (5, 3)),
        "reentry": actions((0,), (0,), (13, 2), (2, 0), (12,), (0,), (13, 1), (3, 2)),
        "malformed": actions((0,), (14, 3), (5, 1), (11, 0, 88)),
    }

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=20000)
    parser.add_argument("--seed", type=int, default=424242)
    args = parser.parse_args()
    if args.runs < 100 or args.seed <= 0: parser.error("positive seed and at least 100 executions required")
    repo = Path(__file__).resolve().parents[2]
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    def git(*values: str) -> str:
        return subprocess.check_output(["git", *values], cwd=repo, text=True, encoding="utf-8").strip()
    manifest = {"schema": "gamenet.rpc_fuzz.v1", "status": "failed", "commit_sha": git("rev-parse", "HEAD"),
                "working_tree": git("status", "--porcelain=v1"), "workflow_sha": os.getenv("GITHUB_SHA"),
                "run_id": os.getenv("GITHUB_RUN_ID"), "run_attempt": os.getenv("GITHUB_RUN_ATTEMPT"), "targets": []}
    try:
        if manifest["workflow_sha"] and (manifest["workflow_sha"] != manifest["commit_sha"] or manifest["working_tree"]):
            raise ValueError("hosted fuzz requires the exact clean workflow checkout")
        for kind, max_length in KINDS.items():
            folder = output / kind
            if folder.exists(): raise ValueError(f"fresh fuzz evidence directory required: {folder}")
            seeds = folder / "corpus"; seeds.mkdir(parents=True)
            for name, content in corpus(kind).items(): (seeds / name).write_bytes(content)
            crashes = folder / "crashes"; crashes.mkdir(exist_ok=True)
            binary = (args.build_dir / "tests/fuzz" / ("gamenet_fuzz_rpc_" + kind)).resolve()
            command = [str(binary), str(seeds), f"-artifact_prefix={crashes}/", f"-seed={args.seed}",
                       f"-runs={args.runs}", f"-max_len={max_length}", "-timeout=5", "-rss_limit_mb=512", "-print_final_stats=1"]
            (folder / "command.json").write_text(json.dumps(command, indent=2) + "\n", encoding="utf-8")
            environment = {**os.environ, "ASAN_OPTIONS": "halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
            with (folder / "fuzzer.log").open("wb") as log:
                result = subprocess.run(command, cwd=repo, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=600)
            if result.returncode: raise RuntimeError(f"{kind} fuzzer failed: {result.returncode}; retained {folder}")
            verify_log((folder / "fuzzer.log").read_text(encoding="utf-8"), args.runs, args.seed)
            manifest["targets"].append({"kind": kind, "executed_units": args.runs, "seed": args.seed,
                "max_len": max_length, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "files": {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(folder.rglob("*")) if p.is_file()}})
            print(f"{kind}: verified {args.runs} executions, seed {args.seed}", flush=True)
        manifest["status"] = "passed"
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__": main()
