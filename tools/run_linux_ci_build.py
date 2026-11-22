# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Bound Linux Core CI builds and retain compiler output and resource samples."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


EVIDENCE = Path("ci-evidence/linux-build")
PARALLEL_JOBS = 2
BUILD_DEADLINE_SECONDS = 600
SAMPLE_INTERVAL_SECONDS = 15
KILL_GRACE_SECONDS = 10
SCHEMA = "gamenet.core_ci_linux_build.v1"


def read_optional(path):
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return None


def selected_fields(path, names):
    fields = {}
    for line in (read_optional(path) or "").splitlines():
        parts = line.split()
        if parts and parts[0].rstrip(":") in names:
            fields[parts[0].rstrip(":")] = int(parts[1])
    return fields


def cgroup_resources():
    root = Path("/sys/fs/cgroup")
    membership = read_optional(Path("/proc/self/cgroup")) or ""
    path = next((line[3:] for line in membership.splitlines() if line.startswith("0::")), None)
    if path is None:
        return {}
    current = root / path.lstrip("/")
    names = ("cpu.max", "memory.current", "memory.high", "memory.max", "memory.peak",
             "memory.events", "memory.swap.current", "memory.swap.max")
    groups = {}
    for directory in (current, *current.parents):
        if not directory.is_relative_to(root):
            break
        values = {
            name: value for name in names if (value := read_optional(directory / name)) is not None
        }
        if values:
            group = "/" if directory == root else "/" + directory.relative_to(root).as_posix()
            groups[group] = values
    return groups


def sample(event, **details):
    return {
        "schema": SCHEMA,
        "event": event,
        "utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "build_parallelism": PARALLEL_JOBS,
        "cpu_affinity_count": len(os.sched_getaffinity(0)),
        "load_average": os.getloadavg(),
        "memory_kib": selected_fields(
            Path("/proc/meminfo"), {"MemTotal", "MemAvailable", "SwapTotal", "SwapFree"}
        ),
        "host_memory_events": selected_fields(Path("/proc/vmstat"), {"oom_kill"}),
        "memory_pressure": read_optional(Path("/proc/pressure/memory")),
        "workspace_free_bytes": shutil.disk_usage(".").free,
        "cgroup_v2": cgroup_resources(),
        **details,
    }


def preflight():
    identity = ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_JOB",
                "RUNNER_ENVIRONMENT", "ImageOS", "ImageVersion")
    record = sample("preflight", identity={name: os.getenv(name) for name in identity},
                    deadline_seconds=BUILD_DEADLINE_SECONDS)
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(record, indent=2) + "\n"
    (EVIDENCE / "preflight.json").write_text(payload, encoding="utf-8")
    print(payload, end="", flush=True)


def kill_build_group(process):
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def run_build(build_dir, targets, deadline):
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    cache = read_optional(build_dir / "CMakeCache.txt") or ""
    generator = next((line.split("=", 1)[1] for line in cache.splitlines()
                      if line.startswith("CMAKE_GENERATOR:INTERNAL=")), None)
    command = ["cmake", "--build", str(build_dir), "--parallel", str(PARALLEL_JOBS)]
    if targets:
        command.extend(["--target", *targets])
    supervised = ["timeout", "--verbose", f"--kill-after={KILL_GRACE_SECONDS}s", f"{deadline}s", *command]
    log_path = EVIDENCE / f"{build_dir.resolve().name}.log"
    started = time.monotonic()
    with subprocess.Popen(["tee", "-a", str(log_path)], stdin=subprocess.PIPE) as mirror:
        def record(event, **details):
            resource = sample(event, elapsed_seconds=round(time.monotonic() - started, 3), **details)
            mirror.stdin.write(f"CI_BUILD {json.dumps(resource)}\n".encode("utf-8"))
            mirror.stdin.flush()

        record("start", command=command, generator=generator, deadline_seconds=deadline)
        with subprocess.Popen(supervised, start_new_session=True,
                              stdout=mirror.stdin, stderr=subprocess.STDOUT) as process:
            try:
                while True:
                    try:
                        status = process.wait(timeout=SAMPLE_INTERVAL_SECONDS)
                        break
                    except subprocess.TimeoutExpired:
                        record("progress")
            finally:
                if process.poll() is None:
                    kill_build_group(process)
        exit_code = 128 - status if status < 0 else status
        if exit_code in (124, 137):
            # The direct child can exit on TERM while another group member still holds the log pipe.
            kill_build_group(process)
        record("finish", exit_code=exit_code)
        mirror.stdin.close()
        mirror_status = mirror.wait()
    return exit_code or mirror_status


def terminate(_signal, _frame):
    raise SystemExit(143)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("preflight")
    build = actions.add_parser("build")
    build.add_argument("build_dir", type=Path)
    build.add_argument("--target", nargs="+", default=[])
    build.add_argument("--deadline-seconds", type=int, default=BUILD_DEADLINE_SECONDS)
    args = parser.parse_args()
    if args.action == "preflight":
        preflight()
        return 0
    if not 0 < args.deadline_seconds <= BUILD_DEADLINE_SECONDS:
        parser.error(f"the build deadline must be between 1 and {BUILD_DEADLINE_SECONDS} seconds")
    signal.signal(signal.SIGTERM, terminate)
    return run_build(args.build_dir, args.target, args.deadline_seconds)


if __name__ == "__main__":
    sys.exit(main())
