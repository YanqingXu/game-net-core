#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Bounded same-host before/after experiment; NOT a production promotion gate."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import re
import shutil
import statistics
import subprocess
import sys
import xml.etree.ElementTree as ET

import cpu_attribution as cpu
import cpu_variant
import dispatch_budget
import run_acceptance as acceptance

SCHEMA = "gamenet.installed-io-uring-hot-path.v1"
BASELINE = "e391c2f59e1cd0dc9d5cc8a1cbab215b058c488d"
CHANGED = {"src/experimental/io_uring/IoUringCompletionEngine.cc",
           "src/experimental/io_uring/IoUringTcpConnectionHub.cc"}
ARMS = {"epoll": ("baseline", "epoll"), "before": ("baseline", "io_uring"),
        "after": ("candidate", "io_uring")}
PAIRS = 12
PROFILE = {**acceptance.PROFILE, "steady_rounds": cpu.STEADY_ROUNDS,
           "pairs": PAIRS, "slow_pairs": 3, "timer_pairs": 3, "notice_budget": 16,
           "trace_rounds": cpu.TRACE_ROUNDS, "profile_rounds": cpu.PROFILE_ROUNDS}
ENV_KEYS = ("GAMENET_TRACE_STEADY", "GAMENET_PROBE_TIMER", "GAMENET_PUMP_NOTICE_BUDGET")
CONSUMER_FILES = ("acceptance.cpp", "CMakeLists.txt")


def check_identity(manifest, expected_candidate):
    acceptance.require(manifest["baseline_revision"] == BASELINE, "unexpected baseline SHA")
    candidate = manifest["candidate_revision"]
    acceptance.require(isinstance(candidate, str) and re.fullmatch(r"[0-9a-f]{40}", candidate)
                       and candidate != BASELINE and candidate == expected_candidate
                       and manifest["consumer_revision"] == candidate, "unexpected candidate/consumer SHA")


def check_source_delta(paths):
    # Tests/docs/standalone observers may differ; no other runtime, public API,
    # build policy or benchmark-consumer changes belong in this experiment.
    runtime = {path for path in paths if path.startswith(("src/", "include/", "cmake/", "api/"))
               or path in ("CMakeLists.txt", "CMakePresets.json")}
    acceptance.require(runtime == CHANGED, "runtime delta exceeds the two bookkeeping changes")
    acceptance.require(not any(path == f"benchmarks/io_uring/installed_acceptance/{name}"
                               for path in paths for name in CONSUMER_FILES), "consumer changed")


def plan():
    for case, count, arms in (("steady", PAIRS, list(ARMS)),
                              ("slow-close", 3, list(ARMS)),
                              ("timer", 3, ["before", "after"])):
        for pair in range(0 if case == "steady" else 1, count + 1):
            first = pair % len(arms)
            for arm in arms[first:] + arms[:first]:
                yield case, pair, arm


def trial_path(root, case, pair, arm):
    return root / "trials" / case / f"{pair:02d}-{arm}"


def paired_interval(changes):
    acceptance.require(len(changes) == PAIRS, "missing scored pair")
    rng = random.Random(20261123)
    resampled = sorted(statistics.median(rng.choices(changes, k=len(changes))) for _ in range(10000))
    return {"paired_median_percent": statistics.median(changes),
            "bootstrap_95_percent": [resampled[249], resampled[9749]],
            "pairs": changes}


def contracts(path):
    report = ET.parse(path).getroot()
    cases = list(report.iter("testcase"))
    names = {row.attrib["name"] for row in cases}
    acceptance.require(names == cpu_variant.CONTRACTS and len(cases) == len(names)
                       and not any(list(report.iter(tag)) for tag in ("failure", "error", "skipped")),
                       "not all eleven contracts passed")
    return sorted(names)


def summarize(root, cpus):
    rows = []
    paths = set()
    for case, pair, arm in plan():
        path = trial_path(root, case, pair, arm)
        paths.add(path)
        backend = ARMS[arm][1]
        stats = acceptance.sample(path, backend, "slow-close" if case == "slow-close" else "steady",
                                  cpus, cpu.STEADY_ROUNDS)
        raw = cpu.raw_metrics(path)
        if backend == "io_uring":
            acceptance.require(raw["native_pump_notice_budget"] == 16
                               and raw["native_max_consumer_depth"] == 1
                               and 0 < raw["native_notices_dispatched"] <= raw["native_pump_turns"] * 16,
                               "dispatch bound or callback depth changed")
        if case == "timer":
            stats.update(dispatch_budget.read_timer(path, raw))
        else:
            acceptance.require(not (path / "timer.csv").exists(), "timer in a scored trial")
        if case == "steady":
            stats["owner_ns_per_echo"] = stats["owner_cpu_ms"] * 1e6 / (64 * cpu.STEADY_ROUNDS)
        rows.append(dict(case=case, pair=pair, arm=arm, metrics=stats))
    acceptance.require({path for path in (root / "trials").glob("*/*") if path.is_dir()} == paths,
                       "trial inventory changed")
    medians = {}
    for case in ("steady", "slow-close", "timer"):
        medians[case] = {}
        for arm in ARMS:
            samples = [row["metrics"] for row in rows
                       if row["case"] == case and row["arm"] == arm and row["pair"] > 0]
            if samples:
                medians[case][arm] = {key: statistics.median(row[key] for row in samples) for key in samples[0]}
                if case == "timer":
                    medians[case][arm]["worst_timer_us"] = max(row["timer_max_us"] for row in samples)
    changes = {}
    for metric in ("throughput_mib_s", "owner_ns_per_echo", "p99_us"):
        pairs = []
        for pair in range(1, PAIRS + 1):
            values = {row["arm"]: row["metrics"][metric] for row in rows
                      if row["case"] == "steady" and row["pair"] == pair}
            pairs.append(100 * (values["after"] / values["before"] - 1))
        changes[metric] = paired_interval(pairs)
    observations = {}
    for arm in ("before", "after"):
        observations[arm] = {}
        for mode, rounds in (("stacks", cpu.PROFILE_ROUNDS), ("syscalls", cpu.TRACE_ROUNDS)):
            path = root / "profiles" / arm / mode
            acceptance.sample(path, "io_uring", "steady", cpus, rounds)
            observations[arm][mode] = (cpu.profile_stacks(path) if mode == "stacks"
                                       else cpu.profile_syscalls(path, "io_uring"))
    passed = {label: contracts(root / label / "build/contracts.xml")
              for label in ("baseline", "candidate", "sanitizers")}
    return dict(medians=medians, changes=changes, trials=rows, observations=observations, contracts=passed)


def check_builds(root, prefixes):
    for label in ("baseline", "candidate"):
        acceptance.check_build(root / label, prefixes[label])
    for filename in ("package-cache.txt", "consumer-cache.txt"):
        caches = [acceptance.cmake_cache(root / label / "inputs" / filename)
                  for label in ("baseline", "candidate")]
        for key in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_BUILD_TYPE"):
            acceptance.require(caches[0][key] == caches[1][key], f"different compiler/flags: {key}")
    for name in (*CONSUMER_FILES, "libgamenet_core.a"):
        acceptance.require((root / "baseline/inputs" / name).read_bytes()
                           == (root / "candidate/inputs" / name).read_bytes(), f"non-equivalent control: {name}")


def build(source, consumer, work, root, label, flags, *, sanitized=False):
    evidence = root / label
    logs = evidence / "build"
    logs.mkdir(parents=True)
    directory, prefix = work / f"{label}-build", work / f"{label}-prefix"
    def logged(name, command):
        cpu.command_logged(command, logs / f"{name}.log", timeout=600)
    config = ["cmake", "-S", source, "-B", directory, "-G", "Ninja",
              "-DGAMENET_ENABLE_EXPERIMENTAL=ON", "-DCMAKE_INSTALL_LIBDIR=lib",
              f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_CXX_FLAGS={flags}"]
    if sanitized:
        logged("configure", [*config, "-DCMAKE_BUILD_TYPE=Debug", "-DGAMENET_BUILD_TESTING=ON",
                             "-DGAMENET_ENABLE_ASAN_UBSAN=ON"])
    else:
        logged("configure", [*config, "-DCMAKE_BUILD_TYPE=Release", "-DGAMENET_BUILD_TESTING=OFF"])
        logged("package", ["cmake", "--build", directory, "--parallel", "2"])
        logged("install", ["cmake", "--install", directory])
        consumer_build = work / f"{label}-consumer"
        logged("consumer-configure", ["cmake", "-S", consumer, "-B", consumer_build, "-G", "Ninja",
                                      "-DCMAKE_BUILD_TYPE=Release", f"-DGameNetCore_DIR={prefix}/lib/cmake/GameNetCore"])
        logged("consumer-build", ["cmake", "--build", consumer_build, "--parallel", "2", "--verbose"])
        binary = acceptance.stage_build(evidence, directory, consumer_build, prefix)
        for name in CONSUMER_FILES:
            shutil.copyfile(source / "benchmarks/io_uring/installed_acceptance" / name, evidence / "inputs" / name)
        logged("contracts-configure", ["cmake", "-S", source, "-B", directory, "-DGAMENET_BUILD_TESTING=ON"])
    logged("contracts-build", ["cmake", "--build", directory, "--target", "gamenet_io_uring_contracts", "--parallel", "2"])
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    cpu.command_logged(["ctest", "--test-dir", directory, "--output-on-failure", "--timeout", "30",
                        "--no-tests=error", "--output-junit", logs / "contracts.xml", "-R",
                        r"^contract\.io_engine\.test_(io_uring_|cross_backend_tcp_semantics$)"],
                       logs / "contracts.log", env=env)
    contracts(logs / "contracts.xml")
    if sanitized:
        return None
    archive = "libgamenet_experimental_io_uring.a"
    acceptance.require((directory / "src/experimental/io_uring" / archive).read_bytes()
                       == (prefix / "lib" / archive).read_bytes(), "tested experimental library differs from installed library")
    return binary, str(prefix)


def collect(args):
    root, work = args.output.resolve(), args.work.resolve()
    root.mkdir(parents=True)  # Never mix two runs' evidence.
    work.mkdir(parents=True)
    host = acceptance.native_host(root)
    before, after = args.baseline.resolve(), args.candidate.resolve()
    identities = dict(baseline_revision=acceptance.revision(before), candidate_revision=acceptance.revision(after),
                      consumer_revision=acceptance.revision(Path(__file__).resolve().parent))
    check_identity(identities, args.expected_candidate)
    paths = acceptance.command("git", "diff", "--name-only", BASELINE, "HEAD", cwd=after).splitlines()
    check_source_delta(paths)
    (root / "changed-paths.txt").write_text("\n".join(paths) + "\n", encoding="utf-8")
    (root / "runtime.diff").write_text(acceptance.command("git", "diff", BASELINE, "HEAD", "--", *sorted(CHANGED), cwd=after), encoding="utf-8")
    consumer = after / "benchmarks/io_uring/installed_acceptance"
    # __FILE__ appears in Core's diagnostics. Normalize both checkout roots
    # with the SAME flags, so unchanged Core must be byte-identical as well.
    flags = f"-ffile-prefix-map={before}=/gamenet-source -ffile-prefix-map={after}=/gamenet-source"
    binaries, prefixes = {}, {}
    for label, source in (("baseline", before), ("candidate", after)):
        binaries[label], prefixes[label] = build(source, consumer, work, root, label, flags)
    check_builds(root, prefixes)
    build(after, consumer, work, root, "sanitizers", flags, sanitized=True)
    perf, host["perf_version"] = cpu.resolve_perf()
    host["strace_version"] = acceptance.command("strace", "--version").splitlines()[0]
    acceptance.write_json(root / "host.json", host)
    os.sched_setaffinity(0, host["pinned_cpus"])
    env = {key: value for key, value in os.environ.items() if key not in ENV_KEYS}
    for case, pair, arm in plan():
        package, backend = ARMS[arm]
        trial_env = {**env, "GAMENET_PROBE_TIMER": "1"} if case == "timer" else env
        cpu.trial(binaries[package], backend, "slow-close" if case == "slow-close" else "steady",
                  trial_path(root, case, pair, arm), None if case == "slow-close" else cpu.STEADY_ROUNDS,
                  env=trial_env)
    for arm in ("before", "after"):
        for mode, rounds, tool in (("stacks", cpu.PROFILE_ROUNDS, perf), ("syscalls", cpu.TRACE_ROUNDS, "strace")):
            cpu.trial(binaries[ARMS[arm][0]], "io_uring", "steady", root / "profiles" / arm / mode,
                      rounds, mode=mode, tool=tool, env=env)
    result = summarize(root, host["pinned_cpus"])
    acceptance.write_json(root / "summary.json", result)
    acceptance.write_json(root / "manifest.json", dict(schema=SCHEMA, **identities, profile=PROFILE,
                          install_prefixes=prefixes, files=acceptance.file_manifest(root)))
    return result


def verify(root, expected_candidate):
    manifest = cpu.read_json(root / "manifest.json")
    check_identity(manifest, expected_candidate)
    acceptance.require(manifest["schema"] == SCHEMA and manifest["profile"] == PROFILE
                       and manifest["files"] == acceptance.file_manifest(root), "profile, inventory or hash mismatch")
    check_source_delta((root / "changed-paths.txt").read_text().splitlines())
    host, probe = cpu.read_json(root / "host.json"), cpu.read_json(root / "host-probe.json")
    acceptance.check_native_kernel(probe)
    acceptance.require(all(host[key] == value for key, value in probe.items()), "host mismatch")
    cpus = host["pinned_cpus"]
    acceptance.require(len(cpus) == 2 and cpus == sorted(set(cpus))
                       and set(cpus) <= set(host["available_cpus"]), "CPU affinity mismatch")
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
        print("Independent replay: both SHAs, equal consumer/core, raw ledgers, contracts and hashes verified")
    else:
        if not all((args.baseline, args.candidate, args.work, args.output)):
            parser.error("collection requires baseline, candidate, work and output")
        result = collect(args)
    print(json.dumps({key: result[key] for key in ("medians", "changes")}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
