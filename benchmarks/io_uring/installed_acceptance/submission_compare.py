#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Fixed-budget factorial submission/copy experiment with repeated timer controls.

Historical hot_path_compare.py and cpu_variant.py schemas are deliberately not
reinterpreted. Derived arms are exact candidate checkouts with one source file
restored from the fixed baseline, not environment-controlled runtime variants.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys

import cpu_attribution as cpu
import dispatch_budget
import hot_path_compare as previous
import run_acceptance as acceptance

SCHEMA = "gamenet.installed-io-uring-submission-copy.v1"
BASELINE = "7387c1135284847e8957867fbf13fe9f86618b6e"
ENGINE = "src/experimental/io_uring/IoUringCompletionEngine.cc"
PUMP = "src/experimental/io_uring/IoUringEventLoopPump.cc"
CHANGED = {ENGINE, PUMP}
ARMS = {"epoll": ("baseline", "epoll"), "before": ("baseline", "io_uring"),
        "batch": ("batch", "io_uring"), "copy": ("copy", "io_uring"),
        "after": ("candidate", "io_uring")}
LABELS = ("baseline", "batch", "copy", "candidate")
PAIRS, TIMER_PAIRS = 15, 21
PROFILE = {**acceptance.PROFILE, "steady_rounds": cpu.STEADY_ROUNDS,
           "pairs": PAIRS, "slow_pairs": 3, "timer_pairs": TIMER_PAIRS,
           "notice_budget": 16, "timer_period_ns": 1_000_000,
           "trace_rounds": cpu.TRACE_ROUNDS, "profile_rounds": cpu.PROFILE_ROUNDS}


def check_identity(manifest, expected):
    acceptance.require(manifest["baseline_revision"] == BASELINE, "unexpected submission baseline")
    candidate = manifest["candidate_revision"]
    acceptance.require(isinstance(candidate, str) and re.fullmatch(r"[0-9a-f]{40}", candidate)
                       and candidate != BASELINE and candidate == expected
                       and manifest["consumer_revision"] == candidate, "candidate/consumer identity mismatch")


def check_source_delta(paths):
    runtime = {path for path in paths if path.startswith(("src/", "include/", "cmake/", "api/"))
               or path in ("CMakeLists.txt", "CMakePresets.json")}
    acceptance.require(runtime == CHANGED, "submission experiment changed unrelated runtime/API/build policy")
    acceptance.require(not any(f"benchmarks/io_uring/installed_acceptance/{name}" in paths
                               for name in previous.CONSUMER_FILES), "installed consumer changed")


def plan():
    for case, count, arms in (("steady", PAIRS, list(ARMS)), ("slow-close", 3, list(ARMS)),
                              ("timer", TIMER_PAIRS, ["epoll", "before", "after"])):
        for pair in range(0 if case == "steady" else 1, count + 1):
            first = pair % len(arms)
            for arm in arms[first:] + arms[:first]:
                yield case, pair, arm


def paired_interval(changes):
    acceptance.require(len(changes) == PAIRS, "incomplete factorial pairs")
    rng = random.Random(20261124)
    draws = sorted(statistics.median(rng.choices(changes, k=PAIRS)) for _ in range(10000))
    return dict(paired_median_percent=statistics.median(changes),
                bootstrap_95_percent=[draws[249], draws[9749]], pairs=changes)


def check_derivation(root):
    """Offline proof that each single-factor arm differs in exactly that source."""
    for label in LABELS:
        for path in CHANGED:
            expected = "candidate" if label == "candidate" or (label == "batch" and path == PUMP) \
                or (label == "copy" and path == ENGINE) else "baseline"
            acceptance.require((root / label / "source" / path).read_bytes() ==
                               (root / expected / "source" / path).read_bytes(), "factorial source mix mismatch")
    for path in CHANGED:
        acceptance.require((root / "baseline/source" / path).read_bytes() !=
                           (root / "candidate/source" / path).read_bytes(), "factor was not changed")


def check_builds(root, prefixes):
    check_derivation(root)
    for label in LABELS:
        acceptance.check_build(root / label, prefixes[label])
        for filename in ("package-cache.txt", "consumer-cache.txt"):
            base = acceptance.cmake_cache(root / "baseline/inputs" / filename)
            arm = acceptance.cmake_cache(root / label / "inputs" / filename)
            for key in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_BUILD_TYPE"):
                acceptance.require(base[key] == arm[key], f"different compiler/flags: {label}/{key}")
        for name in (*previous.CONSUMER_FILES, "libgamenet_core.a"):
            acceptance.require((root / "baseline/inputs" / name).read_bytes() ==
                               (root / label / "inputs" / name).read_bytes(), f"changed control: {label}/{name}")


def timer_distribution(root, arm):
    """Pool raw observed delays, but keep skipped scheduled ticks in the summary."""
    delays = []
    for pair in range(1, TIMER_PAIRS + 1):
        directory = previous.trial_path(root, "timer", pair, arm)
        raw = cpu.raw_metrics(directory)
        for due, actual in acceptance.get_rows(directory / "timer.csv", "deadline_ns,callback_ns"):
            if raw["measure_begin_ns"] <= int(due) < raw["measure_end_ns"]:
                delays.append((int(actual) - int(due)) / 1000)
    delays.sort()
    acceptance.require(bool(delays), "no timer observations")
    return dict(callbacks=len(delays), p99_us=delays[(99 * len(delays) + 99) // 100 - 1],
                max_us=delays[-1], over_500us=sum(value > 500 for value in delays),
                over_1000us=sum(value > 1000 for value in delays))


def summarize(root, cpus):
    rows, expected_paths = [], set()
    for case, pair, arm in plan():
        directory = previous.trial_path(root, case, pair, arm)
        expected_paths.add(directory)
        backend = ARMS[arm][1]
        stats = acceptance.sample(directory, backend, "slow-close" if case == "slow-close" else "steady",
                                  cpus, cpu.STEADY_ROUNDS)
        raw = cpu.raw_metrics(directory)
        if backend == "io_uring":
            acceptance.require(raw["native_pump_notice_budget"] == 16 and raw["native_max_consumer_depth"] == 1
                               and 0 < raw["native_notices_dispatched"] <= raw["native_pump_turns"] * 16,
                               "dispatch budget or consumer depth changed")
        if case == "timer":
            stats.update(dispatch_budget.read_timer(directory, raw))
        else:
            acceptance.require(not (directory / "timer.csv").exists(), "timer contaminated scored trial")
        if case == "steady":
            stats["owner_ns_per_echo"] = stats["owner_cpu_ms"] * 1e6 / (64 * cpu.STEADY_ROUNDS)
        rows.append(dict(case=case, pair=pair, arm=arm, metrics=stats))
    acceptance.require({path for path in (root / "trials").glob("*/*") if path.is_dir()} == expected_paths,
                       "trial inventory differs from fixed plan")
    medians = {}
    for case in ("steady", "slow-close", "timer"):
        medians[case] = {}
        for arm in ARMS:
            samples = [r["metrics"] for r in rows if r["case"] == case and r["arm"] == arm and r["pair"] > 0]
            if samples:
                medians[case][arm] = {k: statistics.median(r[k] for r in samples) for k in samples[0]}
    changes = {}
    for first, second in (("before", "batch"), ("before", "copy"), ("before", "after"),
                          ("batch", "after"), ("copy", "after")):
        changes[f"{first}_to_{second}"] = {}
        for metric in ("throughput_mib_s", "owner_ns_per_echo", "p99_us"):
            values = {(r["pair"], r["arm"]): r["metrics"][metric] for r in rows if r["case"] == "steady"}
            changes[f"{first}_to_{second}"][metric] = paired_interval([
                100 * (values[(p, second)] / values[(p, first)] - 1) for p in range(1, PAIRS + 1)])
    timers = {}
    for arm in ("epoll", "before", "after"):
        selected = [r["metrics"] for r in rows if r["case"] == "timer" and r["arm"] == arm]
        timers[arm] = {**timer_distribution(root, arm),
                       "scheduled_ticks": sum(r["scheduled_ticks"] for r in selected),
                       "missed_ticks": sum(r["missed_ticks"] for r in selected),
                       "trials_over_500us": sum(r["timer_max_us"] > 500 for r in selected),
                       "trial_maxima_us": [r["timer_max_us"] for r in selected]}
    observations = {}
    for arm in ("before", "batch", "copy", "after"):
        path = root / "profiles" / arm / "syscalls"
        acceptance.sample(path, "io_uring", "steady", cpus, cpu.TRACE_ROUNDS)
        observations[arm] = {"syscalls": cpu.profile_syscalls(path, "io_uring")}
    for arm in ("before", "after"):
        path = root / "profiles" / arm / "stacks"
        acceptance.sample(path, "io_uring", "steady", cpus, cpu.PROFILE_ROUNDS)
        observations[arm]["stacks"] = cpu.profile_stacks(path)
    passed = {label: previous.contracts(root / label / "build/contracts.xml")
              for label in (*LABELS, "sanitizers")}
    return dict(medians=medians, changes=changes, timers=timers, trials=rows, observations=observations, contracts=passed)


def collect(args):
    root, work = args.output.resolve(), args.work.resolve()
    root.mkdir(parents=True)
    work.mkdir(parents=True)
    host = acceptance.native_host(root)
    before, after = args.baseline.resolve(), args.candidate.resolve()
    identities = dict(baseline_revision=acceptance.revision(before), candidate_revision=acceptance.revision(after),
                      consumer_revision=acceptance.revision(Path(__file__).resolve().parent))
    check_identity(identities, args.expected_candidate)
    paths = acceptance.command("git", "diff", "--name-only", BASELINE, "HEAD", cwd=after).splitlines()
    check_source_delta(paths)
    (root / "changed-paths.txt").write_text("\n".join(paths) + "\n", encoding="utf-8")
    (root / "runtime.diff").write_text(acceptance.command("git", "diff", BASELINE, "HEAD", "--", *sorted(CHANGED),
                                                         cwd=after), encoding="utf-8")
    sources = dict(baseline=before, candidate=after)
    for label, restored in (("batch", ENGINE), ("copy", PUMP)):
        source = work / f"{label}-source"
        cpu.command_logged(["git", "clone", "--quiet", "--no-hardlinks", "--local", after, source], root / f"{label}-clone.log")
        content = subprocess.check_output(["git", "show", f"{BASELINE}:{restored}"], cwd=after)
        (source / restored).write_bytes(content)
        changed = acceptance.command("git", "diff", "--name-only", cwd=source).splitlines()
        acceptance.require(changed == [restored] and acceptance.command("git", "rev-parse", "HEAD", cwd=source).strip()
                           == identities["candidate_revision"], "uncontrolled factorial variant")
        sources[label] = source
    source_blobs = {}
    for label, source in sources.items():
        for path in CHANGED:
            content = (source / path).read_bytes()
            archived = root / label / "source" / path
            archived.parent.mkdir(parents=True, exist_ok=True)
            archived.write_bytes(content)
            origin = "candidate" if label == "candidate" or (label == "batch" and path == PUMP) \
                or (label == "copy" and path == ENGINE) else "baseline"
            commit = identities[f"{origin}_revision"]
            blob = acceptance.command("git", "rev-parse", f"{commit}:{path}", cwd=after).strip()
            actual = hashlib.sha1(f"blob {len(content)}\0".encode() + content).hexdigest()
            acceptance.require(actual == blob, "source file does not match its Git blob")
            source_blobs[f"{label}/{path}"] = blob
    consumer = after / "benchmarks/io_uring/installed_acceptance"
    flags = " ".join(f"-ffile-prefix-map={sources[label]}=/gamenet-source" for label in LABELS)
    binaries, prefixes = {}, {}
    for label in LABELS:
        print(f"Building and testing {label}", flush=True)
        binaries[label], prefixes[label] = previous.build(sources[label], consumer, work, root, label, flags)
    check_builds(root, prefixes)
    previous.build(after, consumer, work, root, "sanitizers", flags, sanitized=True)
    perf, host["perf_version"] = cpu.resolve_perf()
    host["strace_version"] = acceptance.command("strace", "--version").splitlines()[0]
    acceptance.write_json(root / "host.json", host)
    os.sched_setaffinity(0, host["pinned_cpus"])
    env = {k: v for k, v in os.environ.items() if k not in previous.ENV_KEYS}
    for case, pair, arm in plan():
        package, backend = ARMS[arm]
        trial_env = {**env, "GAMENET_PROBE_TIMER": "1"} if case == "timer" else env
        print(f"Measuring {case}/{pair:02d}/{arm}", flush=True)
        cpu.trial(binaries[package], backend, "slow-close" if case == "slow-close" else "steady",
                  previous.trial_path(root, case, pair, arm), None if case == "slow-close" else cpu.STEADY_ROUNDS,
                  env=trial_env)
    for arm in ("before", "batch", "copy", "after"):
        cpu.trial(binaries[ARMS[arm][0]], "io_uring", "steady", root / "profiles" / arm / "syscalls",
                  cpu.TRACE_ROUNDS, mode="syscalls", tool="strace", env=env)
    for arm in ("before", "after"):
        cpu.trial(binaries[ARMS[arm][0]], "io_uring", "steady", root / "profiles" / arm / "stacks",
                  cpu.PROFILE_ROUNDS, mode="stacks", tool=perf, env=env)
    result = summarize(root, host["pinned_cpus"])
    acceptance.write_json(root / "summary.json", result)
    acceptance.write_json(root / "manifest.json", dict(schema=SCHEMA, **identities, profile=PROFILE,
                          source_blobs=source_blobs, install_prefixes=prefixes, files=acceptance.file_manifest(root)))
    return result


def verify(root, expected):
    manifest = cpu.read_json(root / "manifest.json")
    check_identity(manifest, expected)
    acceptance.require(manifest["schema"] == SCHEMA and manifest["profile"] == PROFILE
                       and manifest["files"] == acceptance.file_manifest(root), "profile, inventory or hash mismatch")
    check_source_delta((root / "changed-paths.txt").read_text().splitlines())
    expected_blobs = {}
    for label in LABELS:
        for path in CHANGED:
            content = (root / label / "source" / path).read_bytes()
            expected_blobs[f"{label}/{path}"] = hashlib.sha1(f"blob {len(content)}\0".encode() + content).hexdigest()
    acceptance.require(expected_blobs == manifest["source_blobs"], "source Git blob mismatch")
    host, probe = cpu.read_json(root / "host.json"), cpu.read_json(root / "host-probe.json")
    acceptance.check_native_kernel(probe)
    acceptance.require(all(host[k] == v for k, v in probe.items()), "native host evidence mismatch")
    cpus = host["pinned_cpus"]
    acceptance.require(len(cpus) == 2 and cpus == sorted(set(cpus)) and set(cpus) <= set(host["available_cpus"]),
                       "CPU affinity mismatch")
    check_builds(root, manifest["install_prefixes"])
    result = summarize(root, cpus)
    acceptance.require(result == cpu.read_json(root / "summary.json"), "independent replay mismatch")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "work", "output", "verify-output"):
        parser.add_argument(f"--{name}", type=Path)
    parser.add_argument("--expected-candidate", required=True)
    args = parser.parse_args()
    if args.verify_output:
        result = verify(args.verify_output.resolve(), args.expected_candidate)
        print("Independent factorial replay: raw ledgers, source blobs, equal controls, contracts, timers and hashes verified")
    else:
        if not all((args.baseline, args.candidate, args.work, args.output)):
            parser.error("collection requires baseline, candidate, work and output")
        result = collect(args)
    print(json.dumps({k: result[k] for k in ("medians", "changes", "timers")}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
