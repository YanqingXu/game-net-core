#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Run and independently replay the installed io_uring dispatch-budget sweep."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import shutil
import statistics
import subprocess
import sys
import re
from pathlib import Path

import cpu_attribution as profiling
import cpu_variant as variant
import run_acceptance as acceptance


SCHEMA = "gamenet.installed-io-uring-dispatch-budget.v1"
FRESH_SCHEMA = "gamenet.installed-io-uring-dispatch-budget.v2"
PREDECESSOR = {
    "core_revision": "63fe9f16733c8bcdd52b23fe401466f78d28c03d",
    "consumer_revision": "afe540959f8e0d2a75842af5d935f381d0571f5b",
    "run_id": 45000725413,
    "artifact_id": 14745379385,
    "manifest_sha256": "4f479226abb2eff9ea0cae588c63f724e3607da6b74bce4317cf8d6848a22b75",
}
BUDGETS = (16, 32, 64)
ORDERS = tuple(itertools.permutations(BUDGETS))
OBSERVATION_ORDERS = (BUDGETS, BUDGETS[1:] + BUDGETS[:1], BUDGETS[2:] + BUDGETS[:2])
ROUNDS = {"steady": profiling.STEADY_ROUNDS, "timer": 2048, "syscalls": profiling.TRACE_ROUNDS}
TIMER_PERIOD_NS = 1_000_000
PATCH = "callback-send-coalescing.patch"
PATCHED_FILE = "src/experimental/io_uring/IoUringEventLoopPump.cc"


def fingerprint(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"sha256": digest, "bytes": path.stat().st_size}


def predecessor_manifest(path):
    acceptance.require(fingerprint(path)["sha256"] == PREDECESSOR["manifest_sha256"],
                       "original candidate manifest hash mismatch")
    original = profiling.read_json(path)
    acceptance.require(original["schema"] == "gamenet.installed-io-uring-callback-send-coalescing.v1"
                       and all(original[key] == PREDECESSOR[key] for key in ("core_revision", "consumer_revision")),
                       "original candidate source changed")
    return original


def restore_archives(original, prefix, source, root):
    """Restore the exact tested archive bytes after generating the installed layout."""
    prior = predecessor_manifest(original / "manifest.json")
    source_head = acceptance.command("git", "rev-parse", "HEAD", cwd=source)
    source_status = acceptance.command("git", "status", "--porcelain", cwd=source)
    acceptance.require(source_head == PREDECESSOR["core_revision"]
                       and source_status == f"M {PATCHED_FILE}", "unexpected package source delta")
    patch = Path(__file__).resolve().with_name(PATCH)
    # Git stores the artifact patch with LF, independent of the consumer checkout.
    patch_object = subprocess.check_output(["git", "show", f"HEAD:{patch.relative_to(patch.parents[3]).as_posix()}"],
                                           cwd=patch.parent)
    acceptance.require(hashlib.sha256(patch_object).hexdigest()
                       == prior["files"][f"variant/{PATCH}"]["sha256"], "coalescing patch differs from prior run")
    delta = subprocess.check_output(["git", "diff", "--no-ext-diff", "--no-textconv", "--", PATCHED_FILE],
                                    cwd=source)
    acceptance.require(hashlib.sha256(delta).hexdigest()
                       == prior["files"]["variant/source.diff"]["sha256"], "installed source delta differs")
    (root / "package-source.diff").write_bytes(delta)
    for name in acceptance.ARCHIVES:
        member = f"variant/inputs/{name}"
        payload = original / member
        acceptance.require(fingerprint(payload) == prior["files"][member], f"original archive hash mismatch: {name}")
        shutil.copyfile(payload, prefix / "lib" / name)
    shutil.copyfile(original / "manifest.json", root / "predecessor-manifest.json")
    print(f"Reused both installed Release archives from Actions {PREDECESSOR['run_id']}", flush=True)


def prepare_fresh_variant(original, root, expected_source, consumer):
    """Use this run's fully replayed variant; never silently replace the historical v1 pin."""
    variant.verify(original)
    prior = profiling.read_json(original / "manifest.json")
    acceptance.check_revisions(prior, expected_source, consumer)
    acceptance.require(expected_source == consumer, "fresh experiment must use one SHA")
    shutil.copyfile(original / "manifest.json", root / "predecessor-manifest.json")
    shutil.copyfile(original / "variant/source.diff", root / "package-source.diff")
    shutil.copyfile(original / "variant" / PATCH, root / PATCH)
    return {"core_revision": prior["core_revision"], "consumer_revision": consumer,
            "manifest_sha256": fingerprint(root / "predecessor-manifest.json")["sha256"]}


def check_install(root, prefix, fresh_predecessor=None):
    acceptance.check_build(root, prefix)
    if fresh_predecessor is None:
        prior = predecessor_manifest(root / "predecessor-manifest.json")
    else:
        path = root / "predecessor-manifest.json"
        acceptance.require(fingerprint(path)["sha256"] == fresh_predecessor["manifest_sha256"],
                           "fresh predecessor manifest hash mismatch")
        prior = profiling.read_json(path)
        acceptance.require(prior["schema"] == variant.SCHEMA, "unexpected predecessor schema")
        acceptance.check_revisions(prior, fresh_predecessor["core_revision"],
                                   fresh_predecessor["consumer_revision"])
        acceptance.require(prior["core_revision"] == prior["consumer_revision"],
                           "fresh predecessor mixes checkout identities")
        acceptance.require(fingerprint(root / PATCH) == prior["files"][f"variant/{PATCH}"],
                           "fresh patch hash mismatch")
    acceptance.require(fingerprint(root / "package-source.diff") == prior["files"]["variant/source.diff"],
                       "replayed package source delta differs from prior run")
    for name in acceptance.ARCHIVES:
        acceptance.require(fingerprint(root / "inputs" / name) == prior["files"][f"variant/inputs/{name}"],
                           f"linked archive {name} differs from the tested coalesced Release archive")
    if fresh_predecessor is not None:
        name = "installed_io_uring_acceptance"
        acceptance.require(fingerprint(root / "inputs" / name) == prior["files"][f"variant/inputs/{name}"],
                           "dispatch consumer differs from the tested variant consumer")


def trial_plan():
    for block, order in enumerate((BUDGETS[::-1],) + ORDERS):
        for budget in order:
            yield "steady", block, budget
    for case in ("timer", "syscalls", "slow-close"):
        for block, order in enumerate(OBSERVATION_ORDERS, start=1):
            for budget in order:
                yield case, block, budget


def trial_path(root, case, block, budget):
    return root / "trials" / case / f"{block:02d}-{budget}"


def read_timer(directory, raw):
    period = raw["timer_period_ns"]
    begin, end = raw["measure_begin_ns"], raw["measure_end_ns"]
    started, first = raw["timer_started_ns"], raw["timer_first_deadline_ns"]
    cancelled, cancelled_deadline = raw["timer_cancelled_ns"], raw["timer_cancelled_deadline_ns"]
    acceptance.require(period == TIMER_PERIOD_NS and first == started + period
                       and started <= begin < end <= cancelled
                       and raw["stop_requested_ns"] >= cancelled_deadline + 2 * period,
                       f"unexpected timer lifetime in {directory}")

    deadline = first
    previous_callback = 0
    delays = []
    for expected, actual in acceptance.get_rows(directory / "timer.csv", "deadline_ns,callback_ns"):
        due, ran = int(expected), int(actual)
        acceptance.require(due == deadline and previous_callback < ran < cancelled and due <= ran,
                           f"invalid timer cadence in {directory}")
        if begin <= due < end:
            delays.append(ran - due)
        deadline += (1 + (ran - due) // period) * period
        previous_callback = ran
    acceptance.require(deadline == cancelled_deadline, "timer cancellation refers to a different deadline")
    first_in_window = first + max(0, (begin - first + period - 1) // period) * period
    scheduled = (end - 1 - first_in_window) // period + 1
    acceptance.require(0 < len(delays) <= scheduled, "timer failed to make measurable progress")
    delays.sort()
    p99_index = (99 * len(delays) + 99) // 100 - 1
    return {
        "scheduled_ticks": scheduled,
        "callbacks": len(delays),
        "missed_ticks": scheduled - len(delays),
        "missed_pct": 100 * (scheduled - len(delays)) / scheduled,
        "timer_p50_us": statistics.median(delays) / 1000,
        "timer_p99_us": delays[p99_index] / 1000,
        "timer_max_us": delays[-1] / 1000,
    }


def read_trial(root, case, block, budget, cpus):
    directory = trial_path(root, case, block, budget)
    scenario = "slow-close" if case == "slow-close" else "steady"
    stats = acceptance.sample(directory, "io_uring", scenario, cpus,
                              ROUNDS.get(case, acceptance.PROFILE["steady_rounds"]))
    settlement = {key: stats[key] for key in
                  ("accepted_frames", "delivered_frames", "closed_peers", "native_operations_retired")}
    raw = profiling.raw_metrics(directory)
    turns = raw["native_pump_turns"]
    signals = raw["native_pump_continuations"]
    notices = raw["native_notices_dispatched"]
    acceptance.require(raw["native_pump_notice_budget"] == budget and turns > 0
                       and 0 < notices <= turns * budget and raw["native_max_consumer_depth"] == 1,
                       f"pump did not observe its configured bound or callback depth in {directory}")
    if case == "steady":
        echoes = acceptance.PROFILE["connections"] * ROUNDS[case]
        stats.update({
            "owner_cpu_us_per_echo": stats["owner_cpu_ms"] * 1000 / echoes,
            # The public stop summary covers warmup, steady echoes and retirement.
            "pump_turns": turns,
            "pump_continuations": signals,
            "notices_per_turn": notices / turns,
            "pump_turns_per_1000_echoes": 1000 * turns / stats["delivered_frames"],
            "pump_continuations_per_1000_echoes": 1000 * signals / stats["delivered_frames"],
        })
    if case == "syscalls":
        trace = profiling.profile_syscalls(directory, "io_uring")
        stats = {**trace, "total_syscalls": sum(trace["syscalls"].values()),
                 "total_syscalls_per_1000_echoes": 1000 * sum(trace["syscalls"].values()) / trace["echoes"]}
    if case == "timer":
        stats = read_timer(directory, raw)
    else:
        acceptance.require(not (directory / "timer.csv").exists(), f"unexpected timer in {directory}")
    return {"case": case, "block": block, "budget": budget, "metrics": stats, "settlement": settlement}


def summarize(root, cpus):
    trials = []
    expected_paths = set()
    for case, block, budget in trial_plan():
        expected_paths.add(trial_path(root, case, block, budget))
        try:
            trials.append(read_trial(root, case, block, budget, cpus))
        except (ValueError, KeyError, OSError) as error:
            raise ValueError(f"{case}/{block:02d}-{budget}: {error}") from error
    acceptance.require({path for path in (root / "trials").glob("*/*") if path.is_dir()} == expected_paths,
                       "trial directory inventory changed")
    comparisons = {}
    for case in ("steady", "timer", "syscalls", "slow-close"):
        medians = {}
        for budget in BUDGETS:
            rows = [row["metrics"] for row in trials if row["case"] == case
                    and row["budget"] == budget and row["block"] > 0]
            if case == "syscalls":
                names = sorted(set().union(*(row["syscalls_per_1000_echoes"].keys() for row in rows)))
                medians[str(budget)] = {
                    "total_syscalls_per_1000_echoes": statistics.median(row["total_syscalls_per_1000_echoes"] for row in rows),
                    "sqes_per_enter": statistics.median(row["sqes_per_enter"] for row in rows),
                    "syscalls_per_1000_echoes": {
                        name: statistics.median(row["syscalls_per_1000_echoes"].get(name, 0) for row in rows)
                        for name in names
                    },
                }
            else:
                medians[str(budget)] = {key: statistics.median(row[key] for row in rows) for key in rows[0]}
                if case == "timer":
                    medians[str(budget)]["worst_timer_us"] = max(row["timer_max_us"] for row in rows)
                    medians[str(budget)]["missed_ticks_total"] = sum(row["missed_ticks"] for row in rows)
        comparisons[case] = medians
    settlement = {key: sum(row["settlement"][key] for row in trials)
                  for key in trials[0]["settlement"]}
    return {"comparisons": comparisons, "trials": trials, "settlement": settlement}


def profile():
    return {**acceptance.PROFILE, "steady_rounds": ROUNDS["steady"],
            "timer_rounds": ROUNDS["timer"], "trace_rounds": ROUNDS["syscalls"],
            "timer_period_ns": TIMER_PERIOD_NS, "budgets": list(BUDGETS),
            "trial_order": [list(row) for row in trial_plan()]}


def collect(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    host = acceptance.native_host(root)
    host["strace_version"] = acceptance.command("strace", "--version").splitlines()[0]
    consumer = acceptance.revision(Path(__file__).resolve().parent)
    acceptance.require(consumer == os.getenv("GITHUB_SHA", consumer), "unexpected consumer revision")
    fresh = (prepare_fresh_variant(args.fresh_variant.resolve(), root, args.expected_source, consumer)
             if args.fresh_variant else None)
    prefix = args.prefix.resolve()
    binary = acceptance.stage_build(root, args.package_build.resolve(), args.consumer_build.resolve(), prefix)
    check_install(root, str(prefix), fresh)
    acceptance.write_json(root / "host.json", host)
    cpus = host["pinned_cpus"]
    os.sched_setaffinity(0, cpus)
    for case, block, budget in trial_plan():
        scenario = "slow-close" if case == "slow-close" else "steady"
        env = {key: value for key, value in os.environ.items()
               if key not in ("GAMENET_TRACE_STEADY", "GAMENET_PROBE_TIMER", "GAMENET_PUMP_NOTICE_BUDGET")}
        env["GAMENET_PUMP_NOTICE_BUDGET"] = str(budget)
        if case == "timer":
            env["GAMENET_PROBE_TIMER"] = "1"
        profiling.trial(binary, "io_uring", scenario, trial_path(root, case, block, budget),
                        ROUNDS.get(case), mode="syscalls" if case == "syscalls" else None,
                        tool="strace", env=env, trace_syscalls=None)

    summary = summarize(root, cpus)
    acceptance.write_json(root / "summary.json", summary)
    acceptance.write_json(root / "manifest.json", {
        "schema": FRESH_SCHEMA if fresh else SCHEMA, "predecessor": fresh or PREDECESSOR,
        "core_revision": fresh["core_revision"] if fresh else PREDECESSOR["core_revision"],
        "consumer_revision": consumer,
        "profile": profile(), "install_prefix": str(prefix), "files": acceptance.file_manifest(root),
    })
    return summary


def verify(root):
    manifest = profiling.read_json(root / "manifest.json")
    fresh = manifest["schema"] == FRESH_SCHEMA
    acceptance.require((fresh or (manifest["schema"] == SCHEMA and manifest["predecessor"] == PREDECESSOR))
                       and manifest["profile"] == profile(), "evidence schema, input, or workload mismatch")
    if fresh:
        acceptance.check_revisions(manifest, manifest["predecessor"]["core_revision"],
                                   manifest["predecessor"]["consumer_revision"])
    else:
        acceptance.require(re.fullmatch(r"[0-9a-f]{40}", manifest["consumer_revision"]),
                           "invalid consumer revision")
    acceptance.require(manifest["files"] == acceptance.file_manifest(root), "evidence inventory or SHA-256 mismatch")
    probe = profiling.read_json(root / "host-probe.json")
    host = profiling.read_json(root / "host.json")
    acceptance.check_native_kernel(probe)
    acceptance.require(all(host[key] == value for key, value in probe.items()), "host probe mismatch")
    cpus = host["pinned_cpus"]
    acceptance.require(len(cpus) == 2 and cpus == sorted(set(cpus))
                       and set(cpus) <= set(host["available_cpus"]), "invalid CPU affinity")
    check_install(root, manifest["install_prefix"], manifest["predecessor"] if fresh else None)
    summary = summarize(root, cpus)
    acceptance.require(summary == profiling.read_json(root / "summary.json"), "independent replay differs")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-output", type=Path)
    parser.add_argument("--restore-prior", type=Path)
    parser.add_argument("--fresh-variant", type=Path)
    parser.add_argument("--expected-source")
    parser.add_argument("--expected-consumer")
    for name in ("output", "prefix", "source", "package-build", "consumer-build"):
        parser.add_argument(f"--{name}", type=Path)
    args = parser.parse_args()
    if args.verify_output:
        manifest = profiling.read_json(args.verify_output / "manifest.json")
        acceptance.check_revisions({"core_revision": manifest["predecessor"]["core_revision"],
                                    "consumer_revision": manifest["consumer_revision"]},
                                   args.expected_source, args.expected_consumer)
        summary = verify(args.verify_output.resolve())
        print("Independent replay passed for the restored package, frame ledgers, timers and syscall traces")
    elif args.restore_prior:
        if not all((args.output, args.prefix, args.source)):
            parser.error("archive restore needs output, prefix and patched package checkout")
        restore_archives(args.restore_prior.resolve(), args.prefix.resolve(),
                         args.source.resolve(), args.output.resolve())
        return
    else:
        if args.fresh_variant and not args.expected_source:
            parser.error("a fresh variant requires --expected-source")
        if not all((args.output, args.prefix, args.package_build, args.consumer_build)):
            parser.error("collection needs output, prefix and both Release builds")
        summary = collect(args)
    print(json.dumps(summary["comparisons"], indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
