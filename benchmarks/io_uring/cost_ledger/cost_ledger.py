#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Bounded installed-package cost diagnosis and same-host single-factor A/B."""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "installed_acceptance"))
import run_acceptance as a
import cpu_attribution as cpu
import hot_path_compare as hot
import dispatch_budget as dispatch

BASELINE = "6004293e0492819e062326032a5391eda6e83229"
SCHEMA = "gamenet.io-uring-cost-ledger.v1"
ENGINE = "src/experimental/io_uring/IoUringCompletionEngine.cc"
INPUTS = ("src", "include", "cmake", "api", "CMakeLists.txt", "CMakePresets.json",
          "benchmarks/io_uring", ".github/workflows/io-uring-cost-ledger.yml")
PROFILE = dict(blocks=5, rounds=4096, profile_rounds=8192, trace_rounds=512,
               cost_rounds=1024, timer_ms=2000, timer_capacity=32768,
               notice_budget=16, connections=64, frame_bytes=4096)
ENV_KEYS = (*hot.ENV_KEYS, "GAMENET_TIMER_PACE_US", "GAMENET_TIMER_TRACE_DIR",
            "GAMENET_FIXED_TIME_MS", "GAMENET_COST_OUTPUT")

def runtime(path):
    return path.startswith(("src/", "include/", "cmake/", "api/")) or path in ("CMakeLists.txt", "CMakePresets.json")

def source_archive(source, root, label):
    blobs = {}
    for line in a.command("git", "ls-tree", "-r", "HEAD", "--", *INPUTS, cwd=source).splitlines():
        metadata, name = line.split("\t", 1)
        blobs[name] = metadata.split()[2]
        data = subprocess.check_output(["git", "show", f"HEAD:{name}"], cwd=source)
        a.require((source / name).read_bytes() == data, "dirty source input: " + name)
        target = root / "sources" / label / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    a.write_json(root / f"{label}-blobs.json", blobs)

def source_verify(root, label):
    blobs = cpu.read_json(root / f"{label}-blobs.json")
    base = root / "sources" / label
    a.require({p.relative_to(base).as_posix() for p in base.rglob("*") if p.is_file()} == set(blobs), "source inventory")
    for name, digest in blobs.items():
        data = (base / name).read_bytes()
        a.require(hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest() == digest, "source blob hash")
    return blobs

def build(source, consumer, work, root, label, flags, diagnostic=False):
    logs = root / label / "build"
    logs.mkdir(parents=True)
    directory, prefix = work / f"{label}-build", work / f"{label}-prefix"
    cb = work / f"{label}-consumer"
    def log(name, args): cpu.command_logged(args, logs / f"{name}.log", timeout=600)
    extra = " -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-memset" if diagnostic else ""
    log("configure", ["cmake", "-S", source, "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DGAMENET_BUILD_TESTING=OFF", "-DGAMENET_ENABLE_EXPERIMENTAL=ON", "-DCMAKE_INSTALL_LIBDIR=lib",
        f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_CXX_FLAGS={flags}{extra}"])
    log("package", ["cmake", "--build", directory, "--parallel", "2"])
    log("install", ["cmake", "--install", directory])
    log("consumer-configure", ["cmake", "-S", consumer, "-B", cb, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        f"-DGameNetCore_DIR={prefix}/lib/cmake/GameNetCore", f"-DCMAKE_CXX_FLAGS={flags}{extra}",
        f"-DGAMENET_COST_DIAGNOSTICS={'ON' if diagnostic else 'OFF'}"])
    log("consumer-build", ["cmake", "--build", cb, "--parallel", "2", "--verbose"])
    binary = a.stage_build(root / label, directory, cb, prefix)
    if not diagnostic:
        log("contracts-configure", ["cmake", "-S", source, "-B", directory, "-DGAMENET_BUILD_TESTING=ON"])
        log("contracts-build", ["cmake", "--build", directory, "--target", "gamenet_io_uring_contracts", "--parallel", "2"])
        log("contracts", ["ctest", "--test-dir", directory, "--output-on-failure", "--timeout", "30", "--no-tests=error",
            "--output-junit", logs / "contracts.xml", "-R", r"^contract\.io_engine\.test_(io_uring_|cross_backend_tcp_semantics$)"])
        hot.contracts(logs / "contracts.xml")
    return binary, str(prefix)

def labels(stage): return ["before"] if stage == "diagnose" else ["before", "after"]

def plan(stage):
    arms = [(label, backend) for label in labels(stage) for backend in a.BACKENDS]
    for case in ("work", "saturated", "paced"):
        for block in range(0 if case == "work" else 1, PROFILE["blocks"] + 1):
            shift = block % len(arms)
            for label, backend in arms[shift:] + arms[:shift]:
                yield case, block, label, backend

def trial_path(root, row):
    case, block, label, backend = row
    return root / "trials" / case / f"{block:02d}-{label}-{backend}"

def leaf_context(frames):
    if not frames or frames[0][0].startswith("["): return "unknown", "unknown"
    if "kernel" in frames[0][1] or "vmlinux" in frames[0][1]:
        symbols = " ".join(sym for sym, _ in frames)
        if re.search(r"\b(?:tcp_|__tcp_|ip_|__ip_)", symbols): category = "kernel_tcp_ip"
        elif re.search(r"\bio_(?:uring|submit|issue|recv|send|poll)", symbols): category = "kernel_io_uring"
        elif re.search(r"ep_poll|epoll|schedule|finish_task_switch", symbols): category = "kernel_poll_schedule"
        else: category = "kernel_other"
        return "kernel", category
    if re.search(r"malloc|operator new|(?:^|\W)free|memcpy|memmove|memset", frames[0][0]):
        return "user", "allocation_copy_leaf"
    for symbol, _ in frames:
        for term, category in (("IoUringCompletionEngine", "engine"), ("IoUringTcpConnectionAdapter", "adapter"),
                               ("AdapterSend", "adapter"), ("AdapterCommands", "adapter"), ("CommandInLoop", "adapter"),
                               ("IoUringTcpConnectionHub", "hub"), ("IoUringEventLoopPump", "pump")):
            if term in symbol: return "user", category
        if "gamenet::net::" in symbol: return "user", "core"
        if "Consumer<" in symbol: return "user", "workload"
    return "user", "user_other"

def disjoint_stacks(path):
    m = cpu.raw_metrics(path)
    domains, contexts = Counter(), Counter()
    frames, active = [], False
    def flush():
        if active:
            domain, context = leaf_context(frames)
            domains[domain] += 1; contexts[context] += 1
    for line in (path / "stacks.txt").read_text(errors="replace").splitlines():
        header = cpu.PERF_HEADER.match(line)
        if header:
            flush(); frames = []
            pid, tid, sec, nano = header.groups()
            t = int(sec)*10**9 + int(nano.ljust(9, "0"))
            active = int(pid) == int(tid) == m["owner_tid"] and m["measure_begin_ns"] <= t <= m["measure_end_ns"]
        elif active and (frame := cpu.PERF_FRAME.match(line)):
            frames.append((re.sub(r"\+0x[\da-f]+$", "", frame[1]), frame[2]))
    flush()
    total = sum(domains.values())
    a.require(total >= 200 and sum(contexts.values()) == total, "insufficient/disjoint perf samples")
    return dict(samples=total, domains=dict(domains), contexts=dict(contexts),
                context_percent={k: 100*v/total for k, v in contexts.items()})

def cost_result(path):
    rows = list(a.get_rows(path / "cost.csv", "kind,pc,calls,bytes"))
    a.require(rows and len(rows) <= 8192 and len({(r[0], r[1]) for r in rows}) == len(rows), "cost inventory")
    symbols = cpu.read_json(path / "symbols.json")
    a.require(set(symbols) == {r[1] for r in rows}, "cost symbol inventory")
    totals, sites = {}, []
    for kind, pc, calls, size in rows:
        a.require(kind in ("0", "1", "2", "3", "4") and int(calls) > 0 and int(size) >= 0, "invalid cost row")
        name = ("new", "delete", "memcpy", "memmove", "memset")[int(kind)]
        total = totals.setdefault(name, dict(calls=0, bytes=0))
        total["calls"] += int(calls); total["bytes"] += int(size)
        sites.append(dict(kind=name, pc=pc, calls=int(calls), bytes=int(size), symbol=symbols[pc]))
    frames = PROFILE["cost_rounds"] * 64
    return dict(frames=frames, totals=totals,
                per_echo={k: {m: v/frames for m, v in row.items()} for k, row in totals.items()},
                sites=sorted(sites, key=lambda r: r["bytes"], reverse=True),
                diagnostic_usage={k: int(v) for k, v in a.get_rows(path / "usage.csv", "")})

def summarize(root, stage, cpus):
    rows, expected = [], set()
    for row in plan(stage):
        case, block, label, backend = row
        path = trial_path(root, row); expected.add(path)
        m = cpu.raw_metrics(path)
        count = PROFILE["rounds"] if case == "work" else m["steady_rounds"]
        if case != "work":
            a.require(0 < count <= PROFILE["timer_capacity"] and m["fixed_time_ns"] == PROFILE["timer_ms"]*10**6
                      and m["measure_end_ns"] - m["measure_begin_ns"] >= m["fixed_time_ns"], "fixed-time boundary")
        else: a.require(m["fixed_time_ns"] == 0 and not (path / "timer.csv").exists(), "instrumented scored work")
        result = a.sample(path, backend, "steady", cpus, count)
        result["owner_ns_per_echo"] = result["owner_cpu_ms"]*1e6 / (64*count)
        if case != "work":
            a.require(m["timer_pace_us"] == (1000 if case == "paced" else 0), "pacing mismatch")
            result.update(dispatch.read_timer(path, m))
        if backend == "io_uring":
            a.require(m["native_pump_notice_budget"] == 16 and m["native_max_consumer_depth"] == 1, "budget/depth changed")
        rows.append(dict(case=case, block=block, package=label, backend=backend, metrics=result))
    a.require({p for p in (root / "trials").glob("*/*") if p.is_dir()} == expected, "trial inventory mismatch")
    observations = {}
    for label in labels(stage):
        for backend in a.BACKENDS:
            base = root / "profiles" / label / backend
            entry = {}
            for mode, rounds in (("stacks", 8192), ("syscalls", 512), ("cost", 1024)):
                a.sample(base / mode, backend, "steady", cpus, rounds)
                entry[mode] = (disjoint_stacks(base / mode) if mode == "stacks" else
                    cpu.profile_syscalls(base / mode, backend) if mode == "syscalls" else cost_result(base / mode))
            entry["slow_close"] = a.sample(root / "slow" / label / backend, backend, "slow-close", cpus)
            observations[f"{label}-{backend}"] = entry
    medians = {}
    for case in ("work", "saturated", "paced"):
        for label in labels(stage):
            for backend in a.BACKENDS:
                samples = [r["metrics"] for r in rows if r["case"] == case and r["package"] == label and r["backend"] == backend and r["block"] > 0]
                medians[f"{case}-{label}-{backend}"] = {key: statistics.median(s[key] for s in samples) for key in samples[0]}
    changes = {}
    if stage == "compare":
        for backend in a.BACKENDS:
            for metric in ("throughput_mib_s", "owner_ns_per_echo", "p99_us"):
                pairs = []
                for block in range(1, 6):
                    values = {r["package"]: r["metrics"][metric] for r in rows if r["case"] == "work" and r["backend"] == backend and r["block"] == block}
                    pairs.append(100*(values["after"]/values["before"]-1))
                changes[f"{backend}-{metric}"] = dict(pairs=pairs, median_percent=statistics.median(pairs))
    return dict(medians=medians, changes=changes, trials=rows, observations=observations)

def verify(root, expected):
    m = cpu.read_json(root / "manifest.json")
    a.require(m["schema"] == SCHEMA and m["profile"] == PROFILE and m["baseline"] == BASELINE and
              m["candidate"] == expected and re.fullmatch(r"[0-9a-f]{40}", expected) and m["stage"] in ("diagnose", "compare"), "identity/profile mismatch")
    a.require(m["files"] == a.file_manifest(root), "manifest hash mismatch")
    before, after = source_verify(root, "baseline"), source_verify(root, "candidate")
    delta = {p for p in set(before)|set(after) if runtime(p) and before.get(p) != after.get(p)}
    a.require(delta == (set() if m["stage"] == "diagnose" else {ENGINE}), "runtime delta mismatch")
    a.check_native_kernel(cpu.read_json(root / "host-probe.json"))
    host = cpu.read_json(root / "host.json")
    a.require(len(set(map(tuple, host["cpu_topology"]))) == 2, "physical CPUs not distinct")
    a.require(set(m["prefixes"]) == {x+s for x in labels(m["stage"]) for s in ("", "-cost")}, "build inventory")
    for label, prefix in m["prefixes"].items():
        a.check_build(root / label, prefix)
        cache = a.cmake_cache(root / label / "inputs/consumer-cache.txt")
        a.require(cache["GAMENET_COST_DIAGNOSTICS"] == ("ON" if label.endswith("-cost") else "OFF"), "observer contaminates score")
        if not label.endswith("-cost"): hot.contracts(root / label / "build/contracts.xml")
    if m["stage"] == "compare":
        for name in ("libgamenet_core.a",):
            a.require((root / "before/inputs" / name).read_bytes() == (root / "after/inputs" / name).read_bytes(), "epoll runtime changed")
        for filename in ("package-cache.txt", "consumer-cache.txt"):
            configs = [a.cmake_cache(root / p / "inputs" / filename) for p in ("before", "after")]
            for key in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_BUILD_TYPE"):
                a.require(configs[0][key] == configs[1][key], "unequal comparison flags")
    result = summarize(root, m["stage"], host["pinned_cpus"])
    a.require(result == cpu.read_json(root / "summary.json"), "summary replay mismatch")
    print("cost ledger verified", flush=True)

def collect(args):
    root, work = args.output.resolve(), args.work.resolve()
    root.mkdir(parents=True); work.mkdir(parents=True)
    before, after = args.baseline.resolve(), args.candidate.resolve()
    a.require(a.revision(before) == BASELINE and a.revision(after) == args.expected_candidate, "checkout identity")
    paths = a.command("git", "diff", "--name-only", BASELINE, "HEAD", cwd=after).splitlines()
    a.require({p for p in paths if runtime(p)} == (set() if args.stage == "diagnose" else {ENGINE}), "runtime scope")
    host = a.native_host(root); a.write_json(root / "host.json", host)
    a.require(len(set(map(tuple, host["cpu_topology"]))) == 2, "physical CPUs not distinct")
    source_archive(before, root, "baseline"); source_archive(after, root, "candidate")
    consumer = after / "benchmarks/io_uring/installed_acceptance"
    flags = f"-g -ffile-prefix-map={before}=/gamenet-source -ffile-prefix-map={after}=/gamenet-source"
    for label in ("before", "after", "before-cost", "after-cost"):
        for kind in ("build", "consumer"):
            flags += f" -ffile-prefix-map={work}/{label}-{kind}=/gamenet-{kind}"
    binaries, prefixes = {}, {}
    for label in labels(args.stage):
        source = before if label == "before" else after
        for diag in (False, True):
            key = label + ("-cost" if diag else "")
            binaries[key], prefixes[key] = build(source, consumer, work, root, key, flags, diag)
    os.sched_setaffinity(0, host["pinned_cpus"])
    env = {k: v for k, v in os.environ.items() if k not in ENV_KEYS}
    for row in plan(args.stage):
        case, block, label, backend = row
        trial_env = dict(env)
        if case != "work": trial_env.update(GAMENET_PROBE_TIMER="1", GAMENET_FIXED_TIME_MS="2000")
        if case == "paced": trial_env["GAMENET_TIMER_PACE_US"] = "1000"
        cpu.trial(binaries[label], backend, "steady", trial_path(root, row), 4096 if case == "work" else None, env=trial_env)
    perf, _ = cpu.resolve_perf()
    for label in labels(args.stage):
        for backend in a.BACKENDS:
            cpu.trial(binaries[label], backend, "slow-close", root / "slow" / label / backend, env=env)
            for mode, rounds, tool in (("stacks", 8192, perf), ("syscalls", 512, "strace"), ("cost", 1024, None)):
                path = root / "profiles" / label / backend / mode
                diagnostic = mode == "cost"
                binary = binaries[label + ("-cost" if diagnostic else "")]
                cpu.trial(binary, backend, "steady", path, rounds, mode=None if diagnostic else mode, tool=tool,
                          env={**env, "GAMENET_COST_OUTPUT": str(path)} if diagnostic else env)
                if diagnostic:
                    pcs = sorted({r[1] for r in a.get_rows(path / "cost.csv", "kind,pc,calls,bytes")})
                    # Full inlining chain, per return call-site. Binary is retained.
                    symbols = {pc: a.command("addr2line", "-f", "-C", "-i", "-e", str(binary), "0x"+pc) for pc in pcs}
                    a.write_json(path / "symbols.json", symbols)
    a.write_json(root / "summary.json", summarize(root, args.stage, host["pinned_cpus"]))
    a.write_json(root / "manifest.json", dict(schema=SCHEMA, profile=PROFILE, stage=args.stage,
        baseline=BASELINE, candidate=args.expected_candidate, prefixes=prefixes, files=a.file_manifest(root)))
    verify(root, args.expected_candidate)

def main():
    p = argparse.ArgumentParser()
    for name in ("baseline", "candidate", "work", "output", "verify-output"): p.add_argument("--"+name, type=Path)
    p.add_argument("--expected-candidate", required=True)
    p.add_argument("--stage", choices=("diagnose", "compare"), default="diagnose")
    args = p.parse_args()
    if args.verify_output: verify(args.verify_output.resolve(), args.expected_candidate)
    else: collect(args)
if __name__ == "__main__": main()
