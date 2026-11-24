#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Collect and replay native Linux installed-package CPU profiles."""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
from collections import Counter
from pathlib import Path

import run_acceptance as acceptance


SCHEMA = "gamenet.installed-io-uring-cpu-attribution.v1"
STEADY_ROUNDS = 4096
PROFILE_ROUNDS = 8192
TRACE_ROUNDS = 512
SYSCALLS = (
    "close,epoll_ctl,epoll_wait,epoll_pwait,epoll_pwait2,eventfd2,futex,"
    "io_uring_enter,io_uring_register,io_uring_setup,poll,ppoll,read,readv,"
    "recvfrom,recvmsg,recvmmsg,sendto,sendmsg,sendmmsg,setsockopt,write,writev"
)
PERF_HEADER = re.compile(r"^\s*(\d+)\s*/\s*(\d+)\s+(\d+)\.(\d{1,9}):\s+cpu-clock:")
PERF_FRAME = re.compile(r"^\s*[\da-f]+\s+(.+?)\s+\(([^)]*)\)\s*$")
SYSCALL_LINE = re.compile(r"^\d+\.\d+\s+(\w+)\((.*)\)\s+=\s+(0x[\da-f]+|-?\d+|\?)")
FRAME_GROUPS = {
    "ring_submission": r"\bio_uring_enter\b|__\w*sys_io_uring_enter|::flushOnce\b",
    "io_uring_pump": r"IoUringEventLoopPumpImpl::(?:driveOneTurn|dispatchNotice)",
    "allocation_or_copy": r"malloc|(?:^|\W)free|operator new|memcpy|memmove|memset|_int_free",
    "epoll_wait": r"sys_epoll_(?:p?wait|pwait2)\b|\bepoll_(?:p?wait|pwait2)\b",
}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def raw_metrics(directory):
    return {key: int(value) for key, value in acceptance.get_rows(directory / "metrics.csv", "")}


def command_logged(command, stdout, stderr=None, *, env=None, timeout=180):
    print(" ".join(map(str, command)), flush=True)
    with stdout.open("wb") as output:
        if stderr:
            with stderr.open("wb") as errors:
                result = subprocess.run(command, stdout=output, stderr=errors, env=env, timeout=timeout)
        else:
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, env=env, timeout=timeout)
    if result.returncode:
        print((stderr or stdout).read_bytes()[-4096:].decode(errors="replace"), file=sys.stderr)
        result.check_returncode()


def trial(binary, backend, scenario, directory, rounds=None, *, mode=None, tool=None,
          env=None, trace_syscalls=SYSCALLS):
    directory.mkdir(parents=True)
    target = [binary, backend, scenario, directory]
    if rounds is not None:
        target.append(str(rounds))
    if mode == "stacks":
        data = directory / "perf.data"
        command_logged(["sudo", "-n", tool, "record", "--clockid", "mono", "--no-inherit",
                        "-e", "cpu-clock", "-F", "499", "--call-graph", "dwarf,8192",
                        "-o", data, "--", *target], directory / "record.log")
        command_logged(["sudo", "-n", tool, "script", "-i", data, "--ns",
                        "-F", "pid,tid,time,event,ip,sym,dso"], directory / "stacks.txt",
                       directory / "script.log")
        subprocess.run(["sudo", "-n", "chmod", "a+r", data], check=True)
    elif mode == "syscalls":
        filter_args = ["-e", f"trace={trace_syscalls}"] if trace_syscalls else []
        trace_env = {**(os.environ if env is None else env), "GAMENET_TRACE_STEADY": "1"}
        command_logged([tool, "-qq", "-ttt", "-s", "32", *filter_args,
                        "-o", directory / "syscalls.txt", "--", *target],
                       directory / "trial.log", env=trace_env)
    else:
        command_logged(target, directory / "trial.log", env=env, timeout=60)


def resolve_perf():
    candidates = [Path("/usr/bin/perf")]
    candidates.extend(sorted(Path("/usr/lib/linux-tools").glob("*/perf"), reverse=True))
    for candidate in candidates:
        if candidate.is_file():
            result = subprocess.run([candidate, "version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    encoding="utf-8", errors="replace", timeout=10)
            if result.returncode == 0 and result.stdout.startswith("perf version "):
                return candidate, result.stdout.strip()
    raise ValueError("no usable perf executable found")


def profile_stacks(directory):
    info = raw_metrics(directory)
    owner = info["owner_tid"]
    begin = info["measure_begin_ns"]
    end = info["measure_end_ns"]
    leaves, own_symbols, groups = Counter(), Counter(), Counter()
    included = False
    frames = []
    sample_count = 0
    classified_leaves = 0

    def finish_sample():
        nonlocal sample_count, classified_leaves
        if not included:
            return
        sample_count += 1
        leaf = frames[0] if frames else "[no stack]"
        leaves[leaf] += 1
        if not leaf.startswith("["):
            classified_leaves += 1
        own_symbols.update(set(symbol for symbol in frames if "gamenet::" in symbol))
        for label, pattern in FRAME_GROUPS.items():
            if any(re.search(pattern, symbol) for symbol in frames):
                groups[label] += 1

    with (directory / "stacks.txt").open(encoding="utf-8", errors="replace") as source:
        for line in source:
            header = PERF_HEADER.match(line)
            if header:
                finish_sample()
                pid, tid, sec, nsec = header.groups()
                at = int(sec) * 10**9 + int(nsec.ljust(9, "0"))
                included = int(pid) == int(tid) == owner and begin <= at <= end
                frames = []
            elif included and (frame := PERF_FRAME.match(line)):
                frames.append(re.sub(r"\+0x[\da-f]+$", "", frame[1]))
    finish_sample()
    acceptance.require(sample_count >= 200 and own_symbols and classified_leaves > sample_count // 2,
                       f"insufficient symbolized owner samples in {directory}: {sample_count}")
    return {
        "owner_tid": owner,
        "measured_samples": sample_count,
        "symbolized_leaf_samples": classified_leaves,
        "leaf_symbols": dict(leaves.most_common(30)),
        "present_anywhere_in_stack": {label: groups[label] for label in FRAME_GROUPS},
        "top_gamenet_symbols_in_stack": dict(own_symbols.most_common(20)),
    }


def profile_syscalls(directory, backend):
    calls, errors, submitted_histogram = Counter(), Counter(), Counter()
    triggered = ended = False
    sqes_submitted = 0
    setup_switches = Counter()
    with (directory / "syscalls.txt").open(encoding="utf-8", errors="replace") as source:
        for line in source:
            if 'write(2, "GAMENET_TRACE_' in line:
                if "GAMENET_TRACE_BEGIN" in line:
                    acceptance.require(not triggered, "duplicate syscall trace begin marker")
                    triggered = True
                elif "GAMENET_TRACE_END" in line:
                    acceptance.require(triggered and not ended, "duplicate or misplaced syscall trace end marker")
                    ended = True
                continue
            match = SYSCALL_LINE.match(line)
            if not match:
                if triggered and not ended:
                    acceptance.require(not line.strip() or re.match(r"^\d+\.\d+\s+---", line),
                                       f"cannot parse syscall trace row: {line[:160]!r}")
                continue
            name, arguments, status = match.groups()
            if not triggered and name == "setsockopt" and status == "0":
                options = arguments.split(", ", 4)
                setup_switches[f"{options[1]}/{options[2]}={options[3]}"] += 1
            if not triggered or ended:
                continue
            calls[name] += 1
            if status.startswith("-") or status == "?":
                errors[name] += 1
            if name == "io_uring_enter":
                requested = int(arguments.split(", ", 2)[1], 0)
                submitted_histogram[requested] += 1
                sqes_submitted += max(int(status), 0) if status != "?" else 0
    acceptance.require(triggered and ended and calls, f"incomplete owner syscall trace for {backend}")
    if backend == "io_uring":
        acceptance.require(calls["io_uring_enter"] > 0 and sqes_submitted > 0, "missing native SQE submissions")
    else:
        acceptance.require(not calls["io_uring_enter"] and calls["epoll_wait"] > 0,
                           "unexpected epoll syscall trace")
    echoes = TRACE_ROUNDS * acceptance.PROFILE["connections"]
    return {
        "echoes": echoes,
        "syscalls": dict(calls),
        "errors": dict(errors),
        "syscalls_per_1000_echoes": {key: 1000 * value / echoes for key, value in calls.items()},
        "requested_sqes_histogram": {str(size): count for size, count in sorted(submitted_histogram.items())},
        "sqes_submitted": sqes_submitted,
        "sqes_per_enter": sqes_submitted / calls["io_uring_enter"] if calls["io_uring_enter"] else None,
        "startup_socket_options": dict(setup_switches),
    }


def baseline_plan():
    for pair in range(acceptance.PAIRS + 1):
        order = acceptance.BACKENDS if pair % 2 else acceptance.BACKENDS[::-1]
        for backend in order:
            yield pair, backend


def steady_path(root, pair, backend):
    return acceptance.trial_path(root, "steady", pair, backend)


def collect_summary(root, cpus):
    steady, slow, observations = [], {}, {}
    for pair, backend in baseline_plan():
        metrics = acceptance.sample(steady_path(root, pair, backend), backend, "steady", cpus, STEADY_ROUNDS)
        metrics["owner_ns_per_echo"] = metrics["owner_cpu_ms"] * 1e6 / (STEADY_ROUNDS * acceptance.PROFILE["connections"])
        steady.append({"pair": pair, "backend": backend, "metrics": metrics})
    for backend in acceptance.BACKENDS:
        slow[backend] = acceptance.sample(root / "trials" / "slow-close" / backend, backend, "slow-close", cpus)
        base = root / "profiles" / backend
        observations[backend] = {
            "stacks_trial": acceptance.sample(base / "stacks", backend, "steady", cpus, PROFILE_ROUNDS),
            "stacks": profile_stacks(base / "stacks"),
            "trace_trial": acceptance.sample(base / "syscalls", backend, "steady", cpus, TRACE_ROUNDS),
            "syscalls": profile_syscalls(base / "syscalls", backend),
        }
    medians = {}
    for backend in acceptance.BACKENDS:
        rows = [row["metrics"] for row in steady if row["backend"] == backend and row["pair"] != 0]
        medians[backend] = {key: statistics.median(row[key] for row in rows) for key in rows[0]}
    return {"steady_medians": medians, "steady": steady, "slow_close": slow, "observations": observations}


def run(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    host = acceptance.native_host(root)
    core = acceptance.revision(args.source.resolve())
    consumer = acceptance.revision(Path(__file__).resolve().parent)
    acceptance.require(core == args.expected_source and consumer == os.getenv("GITHUB_SHA", consumer),
                       "unexpected package or benchmark checkout")
    host["perf_event_paranoid"] = Path("/proc/sys/kernel/perf_event_paranoid").read_text().strip()
    perf, host["perf_version"] = resolve_perf()
    host["strace_version"] = acceptance.command("strace", "--version").splitlines()[0]
    acceptance.write_json(root / "host.json", host)
    prefix = args.prefix.resolve()
    binary = acceptance.stage_build(root, args.package_build.resolve(), args.consumer_build.resolve(), prefix)
    cpus = host["pinned_cpus"]
    os.sched_setaffinity(0, cpus)
    for pair, backend in baseline_plan():
        trial(binary, backend, "steady", steady_path(root, pair, backend), STEADY_ROUNDS)
    for backend in acceptance.BACKENDS:
        trial(binary, backend, "slow-close", root / "trials" / "slow-close" / backend)
        directory = root / "profiles" / backend
        trial(binary, backend, "steady", directory / "stacks", PROFILE_ROUNDS, mode="stacks", tool=perf)
        trial(binary, backend, "steady", directory / "syscalls", TRACE_ROUNDS, mode="syscalls", tool="strace")
    summary = collect_summary(root, cpus)
    acceptance.write_json(root / "summary.json", summary)
    acceptance.write_json(root / "manifest.json", {
        "schema": SCHEMA, "core_revision": core, "consumer_revision": consumer,
        "install_prefix": str(prefix),
        "profile": {**acceptance.PROFILE, "steady_rounds": STEADY_ROUNDS,
                    "profile_rounds": PROFILE_ROUNDS, "trace_rounds": TRACE_ROUNDS},
        "files": acceptance.file_manifest(root),
    })
    return summary


def verify(root):
    manifest = read_json(root / "manifest.json")
    acceptance.require(manifest["schema"] == SCHEMA
                       and manifest["files"] == acceptance.file_manifest(root), "inventory or digest mismatch")
    expected_profile = {**acceptance.PROFILE, "steady_rounds": STEADY_ROUNDS,
                        "profile_rounds": PROFILE_ROUNDS, "trace_rounds": TRACE_ROUNDS}
    acceptance.require(manifest["profile"] == expected_profile, "profile mismatch")
    acceptance.require(all(re.fullmatch(r"[0-9a-f]{40}", manifest[key])
                           for key in ("core_revision", "consumer_revision")), "invalid revision")
    host = read_json(root / "host.json")
    probe = read_json(root / "host-probe.json")
    acceptance.check_native_kernel(probe)
    acceptance.require(all(host[key] == value for key, value in probe.items()), "host probe mismatch")
    cpus = host["pinned_cpus"]
    acceptance.require(len(cpus) == 2 and cpus == sorted(set(cpus))
                       and set(cpus) <= set(host["available_cpus"]), "invalid CPU affinity")
    acceptance.check_build(root, manifest["install_prefix"])
    computed = collect_summary(root, cpus)
    acceptance.require(computed == read_json(root / "summary.json"), "independent replay mismatch")
    return computed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-output", type=Path)
    for name in ("output", "source", "package-build", "consumer-build", "prefix"):
        parser.add_argument(f"--{name}", type=Path)
    parser.add_argument("--expected-source")
    parser.add_argument("--expected-consumer")
    args = parser.parse_args()
    if args.verify_output:
        acceptance.check_revisions(read_json(args.verify_output / "manifest.json"),
                                   args.expected_source, args.expected_consumer)
        summary = verify(args.verify_output.resolve())
        print("Independent replay passed for the frame ledgers, stack samples and syscall traces")
    else:
        if not all((args.output, args.source, args.package_build, args.consumer_build, args.prefix, args.expected_source)):
            parser.error("collection requires the pinned package checkout, builds, prefix and output")
        summary = run(args)
    print(json.dumps({
        "steady": summary["steady_medians"],
        "stacks": {backend: data["stacks"]["present_anywhere_in_stack"]
                   for backend, data in summary["observations"].items()},
        "syscalls": {backend: data["syscalls"]["syscalls_per_1000_echoes"]
                     for backend, data in summary["observations"].items()},
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
