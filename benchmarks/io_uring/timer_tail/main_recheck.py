#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Replayable epoll A/B for the isolated Linux timer fix."""
import argparse
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

BASELINE = "e391c2f59e1cd0dc9d5cc8a1cbab215b058c488d"
CANDIDATE = "16fa18ca55a58d763b02d571f3232a0bc6a5e53b"
OBSERVER = "e7e33266bf285c20c74418bb564df35cafda1266"
PROFILE = {"pairs": 10, "time_ms": 2000, "paced_us": 1000, "idle_callbacks": 3000}
SCHEMA = "gamenet.linux-timer-main-recheck.v1"
ARMS = ("baseline", "candidate")
CASES = ("idle", "paced", "saturated")


def build(source, observer, work, evidence, label):
    root = evidence / label
    logs = root / "build"
    logs.mkdir(parents=True)
    package, consumer, idle, prefix = [work / f"{label}-{part}"
                                      for part in ("package", "consumer", "idle", "prefix")]

    def run(name, args):
        cpu.command_logged(args, logs / f"{name}.log", timeout=600)

    cmake_release = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    installed = [f"-DGameNetCore_DIR={prefix}/lib/cmake/GameNetCore"]
    run("configure", ["cmake", "-S", source, "-B", package, *cmake_release,
                      "-DGAMENET_BUILD_TESTING=OFF", "-DGAMENET_ENABLE_EXPERIMENTAL=ON",
                      "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_INSTALL_PREFIX={prefix}"])
    run("package", ["cmake", "--build", package, "--parallel", "2"])
    run("install", ["cmake", "--install", package])
    run("consumer-configure", ["cmake", "-S", observer / "installed_acceptance",
                               "-B", consumer, *cmake_release, *installed])
    run("consumer-build", ["cmake", "--build", consumer, "--parallel", "2", "--verbose"])
    acceptance.stage_build(root, package, consumer, prefix)
    acceptance.check_build(root, str(prefix))
    run("idle-configure", ["cmake", "-S", observer / "timer_tail", "-B", idle,
                           *cmake_release, *installed])
    run("idle-build", ["cmake", "--build", idle, "--target", "idle", "--verbose"])
    shutil.copyfile(idle / "idle", root / "inputs/idle")
    return idle / "idle", consumer / "installed_io_uring_acceptance"


def plan():
    for block in range(1, PROFILE["pairs"] + 1):
        offset = (block - 1) % len(CASES)
        for case in CASES[offset:] + CASES[:offset]:
            for label in ARMS if block % 2 else ARMS[::-1]:
                yield block, case, label


def trial_dir(root, entry):
    block, case, label = entry
    return root / "trials" / case / f"{block:02d}-{label}"


def replay(root, host):
    rows = []
    cpus = host["pinned_cpus"]
    acceptance.check_native_kernel(host)
    acceptance.require(len(cpus) == 2 and len({tuple(x) for x in host["cpu_topology"]}) == 2,
                       "expected two distinct runner cores")
    acceptance.require({p for p in (root / "trials").glob("*/*") if p.is_dir()}
                       == {trial_dir(root, entry) for entry in plan()}, "trial inventory")
    for entry in plan():
        block, case, label = entry
        path = trial_dir(root, entry)
        raw = cpu.raw_metrics(path)
        if case == "idle":
            result = timer.idle_result(path, "cadence", False)
            ticks = list(acceptance.get_rows(path / "timer.csv", "armed_ns,deadline_ns,callback_ns,delay_us"))
            acceptance.require(raw["owner_cpu"] == cpus[0] and len(ticks) == PROFILE["idle_callbacks"],
                               "idle affinity or cadence count")
            missed = sum((int(fired) - int(due)) // 1_000_000 for _, due, fired, _ in ticks[:-1])
            scores = {"timer_p99_us": result["timer"]["p99_us"], "timer_max_us": result["timer"]["max_us"],
                      "owner_cpu_ms": result["owner_cpu_ms"], "measure_elapsed_ms": result["exposure_ms"],
                      "missed_ticks": missed, "scheduled_ticks": len(ticks) + missed}
        else:
            rounds = raw["steady_rounds"]
            duration = PROFILE["time_ms"] * 1_000_000
            expected_pace = PROFILE["paced_us"] if case == "paced" else 0
            acceptance.require(0 < rounds <= 32768 and raw["fixed_time_ns"] == duration
                               and raw["measure_end_ns"] - raw["measure_begin_ns"] >= duration
                               and raw["timer_pace_us"] == expected_pace, "fixed-time workload")
            scores = acceptance.sample(path, "epoll", "steady", cpus, rounds)
            scores.update(dispatch.read_timer(path, raw))
            scores["owner_ns_per_echo"] = raw["owner_cpu_ns"] / (64 * rounds)
        rows.append({"block": block, "case": case, "package": label, "scores": scores})

    groups, paired = {}, {}
    for case in CASES:
        values = {}
        for label in ARMS:
            group = sorted((r for r in rows if r["case"] == case and r["package"] == label),
                           key=lambda r: r["block"])
            values[label] = [r["scores"] for r in group]
            selected = values[label]
            summaries = {k: statistics.median(row[k] for row in selected) for k in selected[0]}
            summaries["timer_max_us"] = max(row["timer_max_us"] for row in selected)
            for k in ("missed_ticks", "scheduled_ticks", "measure_elapsed_ms"):
                summaries[k] = sum(row[k] for row in selected)
            groups[f"{case}-{label}"] = summaries
        deltas = [after["timer_p99_us"] - before["timer_p99_us"]
                  for before, after in zip(values["baseline"], values["candidate"], strict=True)]
        paired[case] = {"timer_p99_delta_us": deltas, "median_delta_us": statistics.median(deltas)}
    return {"groups": groups, "paired": paired, "trials": rows}


def collect(args, identity):
    root = args.output
    root.mkdir(parents=True)
    args.work.mkdir(parents=True)
    host = acceptance.native_host(root)
    acceptance.require(len({tuple(x) for x in host["cpu_topology"]}) == 2, "separate cores required")
    acceptance.write_json(root / "host.json", host)
    binaries = {label: build(source, args.observer / "benchmarks/io_uring", args.work, root, label)
                for label, source in zip(ARMS, (args.baseline, args.candidate), strict=True)}
    os.sched_setaffinity(0, host["pinned_cpus"])
    env = {k: v for k, v in os.environ.items()
           if k not in (*timer.ENV_KEYS, "GAMENET_FIXED_TIME_MS", "GAMENET_COST_OUTPUT")}
    for entry in plan():
        block, case, label = entry
        print(f"Block {block:02d} {case} {label}", flush=True)
        path = trial_dir(root, entry)
        idle, network = binaries[label]
        if case == "idle":
            path.mkdir(parents=True)
            cpu.command_logged([idle, "cadence", path], path / "trial.log", env=env, timeout=25)
        else:
            trial_env = dict(env, GAMENET_PROBE_TIMER="1", GAMENET_FIXED_TIME_MS=str(PROFILE["time_ms"]))
            if case == "paced":
                trial_env["GAMENET_TIMER_PACE_US"] = str(PROFILE["paced_us"])
            cpu.trial(network, "epoll", "steady", path, env=trial_env)

    for label, (idle, _) in binaries.items():
        probe = root / label / "syscalls"
        probe.mkdir()
        trace = probe / "trace.log"
        cpu.command_logged(["strace", "-qq", "-e", "trace=epoll_wait,epoll_pwait2",
                            "-o", trace, idle, "phase", probe], probe / "run.log", env=env, timeout=25)
        require_syscall(label, trace)
    acceptance.write_json(root / "summary.json", replay(root, host))
    acceptance.write_json(root / "manifest.json", dict(schema=SCHEMA, profile=PROFILE, **identity,
                                                      files=acceptance.file_manifest(root)))


def require_syscall(label, trace):
    lines = trace.read_text().splitlines()
    if label == "baseline":
        acceptance.require(any(line.startswith("epoll_wait(") for line in lines)
                           and all(not line.startswith("epoll_pwait2(") for line in lines), "baseline syscall")
    else:
        acceptance.require(any(line.startswith("epoll_pwait2(") and line.endswith("= 0")
                               for line in lines), "candidate precise wait was not observed")


def verify(args, identity):
    root = args.output
    manifest = cpu.read_json(root / "manifest.json")
    acceptance.require(manifest["schema"] == SCHEMA and manifest["profile"] == PROFILE
                       and all(manifest[k] == value for k, value in identity.items()), "identity and profile")
    acceptance.require(manifest["files"] == acceptance.file_manifest(root), "file hashes or inventory")
    for label in ARMS:
        require_syscall(label, root / label / "syscalls/trace.log")
    summary = replay(root, cpu.read_json(root / "host.json"))
    acceptance.require(summary == cpu.read_json(root / "summary.json"), "raw replay differs from summary")
    print({name: summary["groups"][name] for name in summary["groups"]}, flush=True)
    report = os.getenv("GITHUB_STEP_SUMMARY")
    if report:
        with Path(report).open("a", encoding="utf-8") as stream:
            stream.write("## Linux timer epoll A/B\n\n")
            stream.write(f"Baseline \x60{BASELINE}\x60. Candidate \x60{CANDIDATE}\x60. Observer \x60{OBSERVER}\x60.\n\n")
            stream.write("| Case | Arm | Timer P99 (µs) | Worst (µs) | Missed / scheduled | Throughput (MiB/s) | Owner CPU (ns/echo) |\n")
            stream.write("| --- | --- | ---: | ---: | ---: | ---: | ---: |\n")
            for case in CASES:
                for label in ARMS:
                    row = summary["groups"][f"{case}-{label}"]
                    rate = f"{row['throughput_mib_s']:.2f}" if case != "idle" else "—"
                    cpu_cost = f"{row['owner_ns_per_echo']:.2f}" if case != "idle" else "—"
                    stream.write(f"| {case} | {label} | {row['timer_p99_us']:.3f} | {row['timer_max_us']:.3f} | "
                                 f"{row['missed_ticks']} / {row['scheduled_ticks']} | {rate} | {cpu_cost} |\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("observer", "baseline", "candidate", "work", "output"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    for folder in ("timer_tail", "installed_acceptance"):
        sys.path.insert(0, str(args.observer / "benchmarks/io_uring" / folder))
    import cpu_attribution as cpu
    import dispatch_budget as dispatch
    import run_acceptance as acceptance
    import timer_evidence as timer

    identity = {"baseline": acceptance.revision(args.baseline), "candidate": acceptance.revision(args.candidate),
                "observer": acceptance.revision(args.observer),
                "workflow": acceptance.revision(Path(__file__).resolve().parent)}
    acceptance.require((identity["baseline"], identity["candidate"], identity["observer"])
                       == (BASELINE, CANDIDATE, OBSERVER), "checkout identity")
    delta = acceptance.command("git", "diff", "--name-only", BASELINE, "HEAD", cwd=args.candidate)
    acceptance.require(acceptance.command("git", "rev-parse", "HEAD^", cwd=args.candidate) == BASELINE
                       and set(delta.splitlines()) == timer.RUNTIME_DELTA | {
                           "intents/architecture/io_engine.intent.md", "intents/modules/timer_queue.intent.md",
                           "tests/contract/io_engine/test_readiness_engine.cpp"}, "unexpected candidate delta")
    if args.verify:
        verify(args, identity)
    else:
        collect(args, identity)
