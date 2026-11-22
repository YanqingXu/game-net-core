#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Compare same-host Release packages differing only in callback Send flushing."""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import cpu_attribution as profiling
import run_acceptance as acceptance


SCHEMA = "gamenet.installed-io-uring-callback-send-coalescing.v1"
PATCH = "callback-send-coalescing.patch"
CHANGED_SOURCE = "src/experimental/io_uring/IoUringEventLoopPump.cc"
ARMS = {
    "epoll": "epoll",
    "io_uring_immediate": "io_uring",
    "io_uring_coalesced": "io_uring",
}
SLOW_PAIRS = 5
ECHOES = acceptance.PROFILE["connections"] * profiling.STEADY_ROUNDS


def trial_plan():
    arms = list(ARMS)
    for pair in range(acceptance.PAIRS + 1):
        first = pair % len(arms)
        for arm in arms[first:] + arms[:first]:
            yield "steady", pair, arm
    for pair in range(1, SLOW_PAIRS + 1):
        options = ("io_uring_immediate", "io_uring_coalesced")
        for arm in options if pair % 2 else options[::-1]:
            yield "slow-close", pair, arm


def trial_path(root, scenario, pair, arm):
    return root / "trials" / scenario / f"{pair:02d}-{arm}"


def check_variant_build(root, main_prefix, variant_prefix):
    acceptance.check_build(root, main_prefix)
    acceptance.check_build(root / "variant", variant_prefix)
    for name in ("package-cache.txt", "consumer-cache.txt"):
        core = acceptance.cmake_cache(root / "inputs" / name)
        variant = acceptance.cmake_cache(root / "variant" / "inputs" / name)
        keys = ("CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_BUILD_TYPE")
        acceptance.require(all(core[key] == variant[key] for key in keys), "compiler or flags changed")
    patch = (root / "variant" / PATCH).read_text(encoding="utf-8")
    actual = (root / "variant/source.diff").read_text(encoding="utf-8")
    headers = f"--- a/{CHANGED_SOURCE}\n+++ b/{CHANGED_SOURCE}\n"
    start = actual.index(headers)
    def edits(diff):
        return [line for line in diff.splitlines() if line.startswith(("+", "-"))]

    acceptance.require(patch.startswith(f"--- a/{CHANGED_SOURCE}\n+++ b/{CHANGED_SOURCE}\n")
                       and actual.startswith(f"diff --git a/{CHANGED_SOURCE} b/{CHANGED_SOURCE}\n")
                       and actual.count("\ndiff --git ") == 0 and edits(actual[start:]) == edits(patch),
                       "applied code differs from the isolated Send-flush change")


def validate_contracts(root):
    report = ET.parse(root / "variant/build/io-uring-contracts.xml").getroot()
    names = {test.attrib["name"] for test in report.iter("testcase")}
    acceptance.require(len(names) == 11
                       and all(name.startswith("contract.io_engine.test_") for name in names)
                       and "contract.io_engine.test_cross_backend_tcp_semantics" in names
                       and all(not list(report.iter(tag)) for tag in ("failure", "error", "skipped")),
                       "candidate io_uring contracts did not all pass")
    return sorted(names)


def summarize(root, cpus):
    trials = []
    for scenario, pair, arm in trial_plan():
        raw = acceptance.sample(trial_path(root, scenario, pair, arm), ARMS[arm], scenario,
                                cpus, profiling.STEADY_ROUNDS)
        if scenario == "steady":
            raw["owner_ns_per_echo"] = raw["owner_cpu_ms"] * 1e6 / ECHOES
        trials.append({"scenario": scenario, "pair": pair, "arm": arm, "metrics": raw})

    def group(scenario):
        result = {}
        for arm in ARMS:
            subset = [trial["metrics"] for trial in trials if trial["arm"] == arm
                      and trial["scenario"] == scenario and trial["pair"] > 0]
            if subset:
                result[arm] = {key: statistics.median(row[key] for row in subset) for key in subset[0]}
        return result

    trace = {}
    for arm in ("io_uring_immediate", "io_uring_coalesced"):
        path = root / "profiles" / arm
        acceptance.sample(path, "io_uring", "steady", cpus, profiling.TRACE_ROUNDS)
        trace[arm] = profiling.profile_syscalls(path, "io_uring")

    pairs = []
    for pair in range(1, acceptance.PAIRS + 1):
        rows = {trial["arm"]: trial["metrics"] for trial in trials
                if trial["scenario"] == "steady" and trial["pair"] == pair}
        original, candidate = rows["io_uring_immediate"], rows["io_uring_coalesced"]
        pairs.append({
            "pair": pair,
            "owner_cpu_change_percent": 100 * (candidate["owner_cpu_ms"] / original["owner_cpu_ms"] - 1),
            "p99_change_percent": 100 * (candidate["p99_us"] / original["p99_us"] - 1),
            "throughput_change_percent": 100 * (candidate["throughput_mib_s"] / original["throughput_mib_s"] - 1),
        })
    return {"steady_medians": group("steady"), "slow_close_medians": group("slow-close"),
            "steady_pair_changes": pairs, "trials": trials, "syscall_traces": trace,
            "variant_contracts": validate_contracts(root)}


def collect(args):
    root = args.output.resolve()
    host = acceptance.native_host(root)
    core = acceptance.revision(args.source.resolve())
    working_variant = args.variant_source.resolve()
    variant = acceptance.command("git", "rev-parse", "HEAD", cwd=working_variant)
    consumer_dir = Path(__file__).resolve().parent
    consumer = acceptance.revision(consumer_dir)
    acceptance.require(core == variant == args.expected_source and consumer == os.getenv("GITHUB_SHA", consumer),
                       "unexpected checkout for the paired comparison")
    status = acceptance.command("git", "status", "--porcelain", cwd=working_variant)
    acceptance.require(status == f"M {CHANGED_SOURCE}", "expected only one modified candidate source file")
    (root / "variant" / PATCH).write_bytes((consumer_dir / PATCH).read_bytes())
    # Preserve the Git diff so replay can compare it to the intended patch.
    actual = subprocess.check_output(["git", "diff", "--no-ext-diff", "--no-textconv", "--", CHANGED_SOURCE],
                                     cwd=working_variant)
    (root / "variant/source.diff").write_bytes(actual)
    acceptance.write_json(root / "host.json", host)

    main_prefix, variant_prefix = args.prefix.resolve(), args.variant_prefix.resolve()
    main_binary = acceptance.stage_build(root, args.package_build.resolve(),
                                         args.consumer_build.resolve(), main_prefix)
    variant_binary = acceptance.stage_build(root / "variant", args.variant_package_build.resolve(),
                                            args.variant_consumer_build.resolve(), variant_prefix)
    check_variant_build(root, str(main_prefix), str(variant_prefix))

    def executable(arm):
        return variant_binary if arm == "io_uring_coalesced" else main_binary

    cpus = host["pinned_cpus"]
    os.sched_setaffinity(0, cpus)
    for scenario, pair, arm in trial_plan():
        rounds = profiling.STEADY_ROUNDS if scenario == "steady" else None
        profiling.trial(executable(arm), ARMS[arm], scenario, trial_path(root, scenario, pair, arm), rounds)
    for arm in ("io_uring_immediate", "io_uring_coalesced"):
        profiling.trial(executable(arm), "io_uring", "steady", root / "profiles" / arm,
                        profiling.TRACE_ROUNDS, mode="syscalls", tool="strace")

    summary = summarize(root, cpus)
    acceptance.write_json(root / "summary.json", summary)
    acceptance.write_json(root / "manifest.json", {
        "schema": SCHEMA, "core_revision": core, "consumer_revision": consumer,
        "main_prefix": str(main_prefix), "variant_prefix": str(variant_prefix),
        "steady_rounds": profiling.STEADY_ROUNDS, "trace_rounds": profiling.TRACE_ROUNDS,
        "steady_pairs": acceptance.PAIRS, "slow_close_pairs": SLOW_PAIRS,
        "files": acceptance.file_manifest(root),
    })
    return summary


def verify(root):
    manifest = profiling.read_json(root / "manifest.json")
    acceptance.require(manifest["schema"] == SCHEMA
                       and manifest["files"] == acceptance.file_manifest(root), "inventory or digest mismatch")
    acceptance.require(all(re.fullmatch(r"[0-9a-f]{40}", manifest[key])
                           for key in ("core_revision", "consumer_revision")), "invalid revision")
    acceptance.require(
        (manifest["steady_rounds"], manifest["trace_rounds"], manifest["steady_pairs"], manifest["slow_close_pairs"])
        == (profiling.STEADY_ROUNDS, profiling.TRACE_ROUNDS, acceptance.PAIRS, SLOW_PAIRS), "workload changed")
    probe = profiling.read_json(root / "host-probe.json")
    host = profiling.read_json(root / "host.json")
    acceptance.check_native_kernel(probe)
    acceptance.require(all(host[key] == value for key, value in probe.items()), "host probe mismatch")
    cpus = host["pinned_cpus"]
    acceptance.require(len(cpus) == 2 and cpus == sorted(set(cpus))
                       and set(cpus) <= set(host["available_cpus"]), "invalid CPU affinity")
    check_variant_build(root, manifest["main_prefix"], manifest["variant_prefix"])
    summary = summarize(root, cpus)
    acceptance.require(summary == profiling.read_json(root / "summary.json"),
                       "variant summary differs from independently replayed evidence")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-output", type=Path)
    arguments = ("output", "source", "package-build", "consumer-build", "prefix",
                 "variant-source", "variant-package-build", "variant-consumer-build", "variant-prefix")
    for name in arguments:
        parser.add_argument(f"--{name}", type=Path)
    parser.add_argument("--expected-source")
    args = parser.parse_args()
    if args.verify_output:
        summary = verify(args.verify_output.resolve())
        print("Independent replay passed for the isolated variant, contracts, ledgers and syscall traces")
    else:
        if not args.expected_source or not all(getattr(args, name.replace("-", "_")) for name in arguments):
            parser.error("collection requires both package checkouts, builds and prefixes")
        summary = collect(args)
    print(json.dumps({key: summary[key] for key in
                      ("steady_medians", "slow_close_medians", "steady_pair_changes", "variant_contracts")}, indent=2))
    for arm, trace in summary["syscall_traces"].items():
        print(arm, trace["syscalls"], trace["requested_sqes_histogram"])


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError, ET.ParseError) as error:
        sys.exit(str(error))
