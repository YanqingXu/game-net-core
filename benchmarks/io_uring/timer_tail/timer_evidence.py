#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Native, bounded timer diagnosis. Trace timings are never performance scores."""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "installed_acceptance"))
import run_acceptance as a
import cpu_attribution as cpu
import hot_path_compare as hot
import dispatch_budget as dispatch

BASELINE = "66a106fe3c972d3642fcc15857b9daaab728e265"
SCHEMA = "gamenet.timer-tail-attribution.v1"
PROFILE = dict(blocks=5, phase_ticks=2000, cadence_ticks=3000, phase_us=[100, 250, 500, 750, 900],
               saturated_rounds=4096, paced_rounds=1024, trace_rounds=1024, pace_us=1000,
               notice_budget=16, connections=64, frame_bytes=4096)
ENV_KEYS = (*hot.ENV_KEYS, "GAMENET_TIMER_PACE_US", "GAMENET_TIMER_TRACE_DIR")
INPUTS = ("src", "include", "cmake", "CMakeLists.txt", "CMakePresets.json", "api",
          "benchmarks/io_uring/timer_tail", "benchmarks/io_uring/installed_acceptance")


def stats(values):
    a.require(bool(values), "empty samples")
    ordered = sorted(values)
    return dict(count=len(values), median_us=statistics.median(values) / 1000,
                p99_us=ordered[min(len(values) - 1, (len(values) * 99 + 99) // 100 - 1)] / 1000,
                max_us=max(values) / 1000, over_500us=sum(x > 500000 for x in values),
                over_1ms=sum(x > 1000000 for x in values))


def parse_observer(path):
    rows = [tuple(map(int, r)) for r in a.get_rows(path, "wall_ns,cpu_ns,kind,value,extra")]
    a.require(rows and len(rows) < 2000000, "empty or overflowed observer")
    waits, messages, stack = [], [], []
    pending = None
    previous = (0, 0)
    for wall, clock, kind, value, extra in rows:
        a.require(wall >= previous[0] and clock >= previous[1], "nonmonotonic observer clock")
        previous = wall, clock
        if kind == 0:
            a.require(pending is None and value >= -1 and extra in (0, 1), "invalid poll entry")
            pending = wall, clock, value, extra
        elif kind == 1:
            a.require(pending is not None and (value >= 0 or extra > 0), "unpaired poll exit")
            start, startcpu, timeout, api = pending
            waits.append((start, wall, clock - startcpu, timeout, value, api))
            pending = None
        elif kind == 2:
            stack.append((wall, clock))
        elif kind == 3:
            a.require(bool(stack), "unpaired message end")
            start, startcpu = stack.pop()
            # Only the outermost span: do not double count nested callbacks.
            if not stack:
                messages.append((start, wall, clock - startcpu))
        else:
            a.require(10 <= kind <= 16 and value >= 0 and extra >= 0, "unknown phase marker")
    a.require(pending is None and not stack and waits, "incomplete observer spans")
    return rows, waits, messages


def intersections(start, end, spans):
    # Spans are ordered/non-overlapping within each category.
    left = bisect.bisect_right(spans, start, key=lambda s: s[1])
    total = 0
    for index in range(left, len(spans)):
        span = spans[index]
        if span[0] >= end:
            break
        total += max(0, min(end, span[1]) - max(start, span[0]))
    return total


def attribution(path, ticks):
    rows, waits, messages = parse_observer(path / "observer.csv")
    # Polls cannot nest in the same owner message callback in this consumer.
    for begin, end, *_ in messages:
        a.require(intersections(begin, end, waits) == 0, "poll nested in consumer callback")
    worst = []
    for due, fired in sorted(ticks, key=lambda t: t[1] - t[0], reverse=True)[:20]:
        wait = intersections(due, fired, waits)
        message = intersections(due, fired, messages)
        a.require(wait + message <= fired - due, "overlapping attribution")
        latest = next((s for s in reversed(waits) if s[0] <= fired), None)
        worst.append(dict(deadline_ns=due, callback_ns=fired, lag_ns=fired - due,
                          poll_ns=wait, message_ns=message, remainder_ns=fired - due - wait - message,
                          last_poll=list(latest) if latest else None))
    return dict(worst=worst, capture_begin_ns=rows[0][0], capture_end_ns=rows[-1][0], poll_calls=len(waits),
                poll_wall_ns=sum(s[1] - s[0] for s in waits), poll_cpu_ns=sum(s[2] for s in waits),
                max_poll_cpu_over_wall_ns=max(0, max(s[2] - (s[1] - s[0]) for s in waits)),
                message_calls=len(messages), message_wall_ns=sum(s[1] - s[0] for s in messages),
                message_cpu_ns=sum(s[2] for s in messages),
                phase_marker_counts={str(kind): sum(r[2] == kind for r in rows) for kind in range(10, 17)},
                caveat="clocks are sampled sequentially; wall minus thread CPU is not a scheduler-cause proof")


def idle_result(path, case, traced):
    raw = cpu.raw_metrics(path)
    rows = [tuple(map(int, r)) for r in a.get_rows(path / "timer.csv", "armed_ns,deadline_ns,callback_ns,delay_us")]
    count = PROFILE[f"{case}_ticks"]
    a.require(len(rows) == count == raw["ticks"] and raw["begin_ns"] < raw["end_ns"], "idle tick count")
    previous = 0
    for i, (armed, due, fired, delay) in enumerate(rows):
        a.require(raw["begin_ns"] <= armed <= fired <= raw["end_ns"] and due <= fired and fired > previous, "idle timer order")
        if case == "phase":
            a.require(delay == PROFILE["phase_us"][i % 5] and due - armed == delay * 1000, "phase case changed")
        else:
            a.require(delay == 1000 and (i == 0 or due == rows[i-1][1] +
                      (1 + (rows[i-1][2] - rows[i-1][1]) // 1000000) * 1000000), "cadence changed")
        previous = fired
    result = dict(timer=stats([fired - due for _, due, fired, _ in rows]),
                  exposure_ms=(raw["end_ns"] - raw["begin_ns"]) / 1e6,
                  owner_cpu_ms=raw["owner_cpu_ns"] / 1e6)
    if case == "phase":
        result["by_delay_us"] = {str(d): stats([f - due for _, due, f, delay in rows if delay == d])
                                 for d in PROFILE["phase_us"]}
    if traced:
        result["attribution"] = attribution(path, [(due, fired) for _, due, fired, _ in rows])
    else:
        a.require(not (path / "observer.csv").exists(), "traced idle score")
    return result


def plan(stage):
    labels = ["baseline"] if stage == "diagnose" else ["baseline", "candidate"]
    for case in ("phase", "cadence", "saturated", "paced", "steady", "slow-close"):
        arms = [(label, backend) for label in labels for backend in
                (["idle"] if case in ("phase", "cadence") else ["epoll", "io_uring"])]
        for block in range(1, PROFILE["blocks"] + 1):
            for label, backend in arms[block % len(arms):] + arms[:block % len(arms)]:
                yield case, block, label, backend, False
    for label in labels:
        for case, backend in (("phase", "idle"), ("cadence", "idle"),
                              ("saturated", "epoll"), ("saturated", "io_uring"),
                              ("paced", "epoll"), ("paced", "io_uring")):
            yield case, 0, label, backend, True


def trial_path(root, row):
    case, block, label, backend, trace = row
    return root / ("traces" if trace else "trials") / case / f"{block:02d}-{label}-{backend}"


def summarize(root, stage, cpus):
    result = []
    inventory = set()
    for row in plan(stage):
        case, block, label, backend, traced = row
        path = trial_path(root, row)
        inventory.add(path)
        if backend == "idle":
            value = idle_result(path, case, traced)
            a.require(cpu.raw_metrics(path)["owner_cpu"] == cpus[0], "idle affinity changed")
        else:
            rounds = PROFILE["trace_rounds"] if traced else PROFILE["paced_rounds" if case == "paced" else "saturated_rounds"]
            value = a.sample(path, backend, "slow-close" if case == "slow-close" else "steady", cpus, rounds)
            raw = cpu.raw_metrics(path)
            if backend == "io_uring":
                a.require(raw["native_pump_notice_budget"] == 16 and raw["native_max_consumer_depth"] == 1 and
                          0 < raw["native_notices_dispatched"] <= raw["native_pump_turns"] * 16, "budget/reentry changed")
            if case in ("saturated", "paced"):
                a.require(raw["timer_pace_us"] == (1000 if case == "paced" else 0), "pacing changed")
                value.update(dispatch.read_timer(path, raw))
                ticks = [tuple(map(int, r)) for r in a.get_rows(path / "timer.csv", "deadline_ns,callback_ns")]
                value["timer"] = stats([f - d for d, f in ticks])
                value["timer_exposure_ms"] = (raw["timer_cancelled_ns"] - raw["timer_started_ns"]) / 1e6
                if traced:
                    value["attribution"] = attribution(path, ticks)
            else:
                a.require(not (path / "timer.csv").exists(), "timer in throughput/close control")
            if not traced:
                a.require(not (path / "observer.csv").exists(), "traced network score")
        result.append(dict(case=case, block=block, package=label, backend=backend, traced=traced, metrics=value))
    actual = {p for group in ("trials", "traces") for p in (root / group).glob("*/*") if p.is_dir()}
    a.require(actual == inventory, "trial inventory changed")
    return result


def archive_sources(source, root, label):
    target = root / "sources" / label
    blobs = {}
    for line in a.command("git", "ls-tree", "-r", "HEAD", "--", *INPUTS, cwd=source).splitlines():
        metadata, name = line.split("\t", 1)
        blob = metadata.split()[2]
        data = subprocess.check_output(["git", "show", f"HEAD:{name}"], cwd=source)
        dest = target / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        blobs[name] = blob
    a.write_json(root / f"{label}-git-blobs.json", blobs)


def verify_sources(root, label):
    blobs = cpu.read_json(root / f"{label}-git-blobs.json")
    directory = root / "sources" / label
    a.require({p.relative_to(directory).as_posix() for p in directory.rglob("*") if p.is_file()} == set(blobs), "source inventory")
    for name, digest in blobs.items():
        data = (directory / name).read_bytes()
        a.require(hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest() == digest, "Git blob mismatch")


def collect(args):
    root, work = args.output.resolve(), args.work.resolve()
    root.mkdir(parents=True)
    work.mkdir(parents=True)
    before, after = args.baseline.resolve(), args.candidate.resolve()
    a.require(a.revision(before) == BASELINE and a.revision(after) == args.expected_candidate, "checkout identity mismatch")
    host = a.native_host(root)
    a.write_json(root / "host.json", host)
    paths = a.command("git", "diff", "--name-only", BASELINE, "HEAD", cwd=after).splitlines()
    runtime = [p for p in paths if p.startswith(("src/", "include/", "cmake/", "api/")) or p in ("CMakeLists.txt", "CMakePresets.json")]
    if args.stage == "diagnose":
        a.require(not runtime, "diagnosis must precede runtime changes")
    (root / "runtime.diff").write_text(a.command("git", "diff", BASELINE, "HEAD", "--", *runtime, cwd=after) if runtime else "", encoding="utf-8")
    archive_sources(before, root, "baseline")
    archive_sources(after, root, "candidate")
    consumer = after / "benchmarks/io_uring/installed_acceptance"
    observer = after / "benchmarks/io_uring/timer_tail"
    labels = ["baseline"] if args.stage == "diagnose" else ["baseline", "candidate"]
    flags = f"-ffile-prefix-map={before}=/gamenet-source -ffile-prefix-map={after}=/gamenet-source"
    binaries, prefixes, probes = {}, {}, {}
    for label in labels:
        binaries[label], prefixes[label] = hot.build(before if label == "baseline" else after,
                                                    consumer, work, root, label, flags)
        # hot.build archives the runtime checkout's observer. Here both arms
        # deliberately use the SAME candidate consumer, including during diagnosis.
        for name in hot.CONSUMER_FILES:
            shutil.copyfile(consumer / name, root / label / "inputs" / name)
        probe = work / f"{label}-timer"
        logs = root / label / "build"
        cpu.command_logged(["cmake", "-S", observer, "-B", probe, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                            f"-DGameNetCore_DIR={prefixes[label]}/lib/cmake/GameNetCore"], logs / "timer-configure.log")
        cpu.command_logged(["cmake", "--build", probe, "--parallel", "2", "--verbose"], logs / "timer-build.log")
        shutil.copyfile(probe / "CMakeCache.txt", root / label / "inputs/timer-cache.txt")
        for name in ("idle", "timer_idle_trace", "timer_network_trace"):
            shutil.copyfile(probe / name, root / label / "inputs" / name)
        probes[label] = probe
    os.sched_setaffinity(0, host["pinned_cpus"])
    env = {k: v for k, v in os.environ.items() if k not in ENV_KEYS}
    for row in plan(args.stage):
        case, block, label, backend, traced = row
        path = trial_path(root, row)
        if backend == "idle":
            path.mkdir(parents=True)
            cpu.command_logged([probes[label] / ("timer_idle_trace" if traced else "idle"), case, path], path / "trial.log", env=env)
        else:
            rounds = PROFILE["trace_rounds"] if traced else PROFILE["paced_rounds" if case == "paced" else "saturated_rounds"]
            trial_env = dict(env)
            if case in ("saturated", "paced"):
                trial_env["GAMENET_PROBE_TIMER"] = "1"
            if case == "paced":
                trial_env["GAMENET_TIMER_PACE_US"] = "1000"
            if traced:
                trial_env["GAMENET_TIMER_TRACE_DIR"] = str(path)
            cpu.trial(probes[label] / "timer_network_trace" if traced else binaries[label], backend,
                      "slow-close" if case == "slow-close" else "steady", path,
                      None if case == "slow-close" else rounds, env=trial_env)
    # No global scheduler knob changes. Probe availability explicitly and keep
    # the error as evidence rather than treating clock subtraction as sched data.
    schedule = root / "scheduler"
    schedule.mkdir()
    try:
        perf, _ = cpu.resolve_perf()
        cpu.command_logged(["sudo", "-n", perf, "record", "-a", "-C", ",".join(map(str, host["pinned_cpus"])),
                            "--clockid", "mono", "-e", "sched:sched_switch", "-e", "sched:sched_wakeup",
                            "-o", schedule / "perf.data", "--", probes[labels[-1]] / "idle", "cadence", schedule],
                           schedule / "record.log", timeout=30)
        cpu.command_logged(["sudo", "-n", perf, "script", "--ns", "-i", schedule / "perf.data"],
                           schedule / "events.txt", schedule / "script.log")
        subprocess.run(["sudo", "-n", "chmod", "a+r", schedule / "perf.data"], check=True)
        status = "captured-unscored-idle"
    except (subprocess.SubprocessError, ValueError, OSError) as error:
        status = "unavailable"
        (schedule / "unavailable.txt").write_text(str(error), encoding="utf-8")
    a.write_json(schedule / "status.json", dict(status=status))
    result = summarize(root, args.stage, host["pinned_cpus"])
    a.write_json(root / "summary.json", result)
    a.write_json(root / "manifest.json", dict(schema=SCHEMA, stage=args.stage, profile=PROFILE,
                  baseline_revision=BASELINE, candidate_revision=args.expected_candidate,
                  consumer_revision=args.expected_candidate, install_prefixes=prefixes,
                  files=a.file_manifest(root)))
    verify(root, args.expected_candidate)


def verify(root, expected):
    manifest = cpu.read_json(root / "manifest.json")
    a.require(manifest["schema"] == SCHEMA and manifest["profile"] == PROFILE and
              manifest["stage"] in ("diagnose", "compare") and manifest["baseline_revision"] == BASELINE and
              manifest["candidate_revision"] == manifest["consumer_revision"] == expected, "identity/profile mismatch")
    a.require(manifest["files"] == a.file_manifest(root), "file inventory/hash mismatch")
    a.check_native_kernel(cpu.read_json(root / "host-probe.json"))
    host = cpu.read_json(root / "host.json")
    for label in ("baseline", "candidate"):
        verify_sources(root, label)
    if manifest["stage"] == "diagnose":
        a.require((root / "runtime.diff").read_text() == "", "diagnosis modified runtime")
    for label, prefix in manifest["install_prefixes"].items():
        a.check_build(root / label, prefix)
        hot.contracts(root / label / "build/contracts.xml")
        cache = a.cmake_cache(root / label / "inputs/timer-cache.txt")
        a.require(cache["GameNetCore_DIR"] == prefix + "/lib/cmake/GameNetCore" and cache["CMAKE_BUILD_TYPE"] == "Release", "probe package mismatch")
        for name in hot.CONSUMER_FILES:
            a.require((root / label / "inputs" / name).read_bytes() ==
                      (root / "sources/candidate/benchmarks/io_uring/installed_acceptance" / name).read_bytes(), "consumer source mismatch")
    a.require(summarize(root, manifest["stage"], host["pinned_cpus"]) == cpu.read_json(root / "summary.json"), "summary replay differs")
    print("timer evidence verified", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline", type=Path)
    p.add_argument("--candidate", type=Path)
    p.add_argument("--work", type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--stage", choices=("diagnose", "compare"), default="diagnose")
    p.add_argument("--verify-output", type=Path)
    p.add_argument("--expected-candidate", required=True)
    args = p.parse_args()
    if args.verify_output:
        verify(args.verify_output, args.expected_candidate)
    else:
        collect(args)

if __name__ == "__main__":
    main()
