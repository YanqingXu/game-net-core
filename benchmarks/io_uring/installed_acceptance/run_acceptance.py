#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Run the installed-package comparison and independently replay its raw ledgers."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
from collections import Counter
from pathlib import Path


SCHEMA = "gamenet.installed-io-uring-acceptance.v1"
PROFILE = {
    "connections": 64,
    "frame_bytes": 4096,
    "warm_rounds": 16,
    "steady_rounds": 256,
    "burst_frames": 2048,
    "slow_hold_ms": 250,
    "output_high_water_bytes": 65536,
    "owner_threads": 1,
    "peer_threads": 1,
}
BACKENDS = ("epoll", "io_uring")
SCENARIOS = ("steady", "slow-close")
PAIRS = 5
ARCHIVES = ("libgamenet_core.a", "libgamenet_experimental_io_uring.a")
CORE_CACHE_KEYS = {
    "CMAKE_BUILD_TYPE": "Release",
    "GAMENET_BUILD_TESTING": "OFF",
    "GAMENET_ENABLE_EXPERIMENTAL": "ON",
    "GAMENET_ENABLE_ASAN_UBSAN": "OFF",
    "GAMENET_ENABLE_TSAN": "OFF",
    "GAMENET_ENABLE_NATIVE_TUNING": "OFF",
    "GAMENET_ENABLE_BENCHMARK_INSTRUMENTATION": "OFF",
}
COMMON_ZERO = (
    "final_active_connections", "final_pending_output_bytes", "final_loop_functors",
    "final_loop_controls", "final_loop_lifecycle", "final_loop_callback_errors",
)
NATIVE_ZERO = (
    "final_active_operations", "final_pending_submissions", "final_pending_cancel_cqes",
    "final_pending_notices", "final_owned_bytes", "final_active_routes", "final_listener_accepts",
    "native_stale_cqes", "native_fallbacks", "native_timeouts",
    "native_callback_or_drive_failures", "send_bytes_discarded",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def file_manifest(root):
    files = {}
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink(), f"symlink in evidence: {path}")
        if path.is_file() and path != root / "manifest.json":
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            files[path.relative_to(root).as_posix()] = {
                "sha256": digest.hexdigest(),
                "bytes": path.stat().st_size,
            }
    return files


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def get_rows(path, header):
    columns = header.split(",") if header else None
    with path.open(encoding="ascii", newline="") as stream:
        reader = csv.reader(stream)
        if columns:
            require(next(reader, None) == columns, f"unexpected CSV header: {path}")
        width = len(columns) if columns else 2
        for row in reader:
            require(len(row) == width, f"incorrect CSV row in {path}")
            yield row


def unique_rows(path, header, key):
    values = {}
    for row in get_rows(path, header):
        identity, columns = key(row)
        require(identity not in values, f"duplicate {identity!r} in {path}")
        values[identity] = tuple(map(int, columns))
    return values


def frame_key(row):
    return (row[0], int(row[1]), int(row[2])), row[3:]


def peer_key(row):
    return int(row[0]), row[1:]


def check_native(directory, metrics, frames_per_peer, peer_count):
    net_ops = metrics["native_accept_submissions"]
    require(net_ops >= peer_count and net_ops == metrics["native_accept_terminals"]
            and metrics["native_listener_socket_closes"] == 1
            and metrics["terminal_socket_closes"] == peer_count,
            "listener accepts or native socket closes did not settle")
    native = unique_rows(
        directory / "native-connections.csv",
        "peer,receive_submissions,receive_terminals,send_submissions,send_terminals,"
        "send_admissions,bytes_sent,bytes_discarded,pending_bytes,pending_segments,"
        "pending_commands,socket_closed",
        peer_key,
    )
    require(native.keys() == frames_per_peer.keys(), "missing native connection summaries")
    for peer, row in native.items():
        receives, recv_done, sends, send_done, admitted, sent_bytes, *cleanup = row
        require(receives > 0 and receives == recv_done and sends > 0 and sends == send_done
                and admitted == frames_per_peer[peer]
                and sent_bytes == admitted * PROFILE["frame_bytes"]
                and cleanup == [0, 0, 0, 0, 1], f"native peer {peer} did not settle: {row}")
        net_ops += receives + sends
    require(net_ops == metrics["native_operations_accepted"] == metrics["native_terminal_notices"]
            == metrics["native_notices_dispatched"],
            "native admission, retirement and dispatch counters disagree")
    require(0 <= metrics["native_cancelled_operations"] <= metrics["native_cancel_cqes"] <= net_ops,
            "native cancellation accounting is inconsistent")
    require(all(metrics[key] == 0 for key in NATIVE_ZERO), "native drain left residue")
    return net_ops


def sample(directory, backend, scenario, cpus):
    metrics = {key: row[0] for key, row in
               unique_rows(directory / "metrics.csv", "", lambda row: (row[0], row[1:])).items()}
    require(all(value >= 0 for value in metrics.values()), "negative metric")
    peer_count = PROFILE["connections"]
    slow = scenario == "slow-close"
    measure_kind = "B" if slow else "E"
    warm = {("W", peer, seq) for peer in range(peer_count)
            for seq in range(PROFILE["warm_rounds"])}
    measured = ({("B", 0, seq) for seq in range(PROFILE["burst_frames"])} if slow else
                {("E", peer, seq) for peer in range(peer_count)
                 for seq in range(PROFILE["steady_rounds"])})
    accepted = unique_rows(directory / "accepted.csv", "kind,peer,sequence,accepted_ns", frame_key)
    received = unique_rows(directory / "received.csv", "kind,peer,sequence,start_ns,received_ns", frame_key)
    closed = unique_rows(directory / "closed.csv",
                         "peer,eof_ns,closed_ns,pending_bytes,reason,native_error", peer_key)
    require(accepted.keys() == received.keys() == warm | measured,
            "accepted and delivered frames differ from the fixed workload")
    require(closed.keys() == set(range(peer_count)), "missing per-peer close or EOF")
    begin, end = metrics["measure_begin_ns"], metrics["measure_end_ns"]
    stop, done = metrics["stop_requested_ns"], metrics["stop_completed_ns"]
    release = metrics["slow_read_released_ns"]
    require(begin > 0 and begin < end and stop < done, "invalid phase times")
    if slow:
        require(begin <= stop < release < end == done
                and release - stop >= PROFILE["slow_hold_ms"] * 1_000_000
                and PROFILE["output_high_water_bytes"] <= metrics["slow_pending_at_stop_bytes"]
                <= PROFILE["burst_frames"] * PROFILE["frame_bytes"],
                "slow-close did not exercise the scheduled hold with actual backpressure")
    else:
        require(end <= stop and release == metrics["slow_pending_at_stop_bytes"] == 0,
                "steady measurement overlapped close or slow-client pressure")
    require(metrics["graceful_close_reason"] == 6, "unexpected package graceful close enum")
    for peer, (eof, closed_at, pending, reason, native_error) in closed.items():
        require(stop <= eof <= closed_at <= done and pending == native_error == 0
                and reason == metrics["graceful_close_reason"], f"unclean close of peer {peer}")

    per_peer = Counter()
    latencies = []
    previous_by_peer = {}
    for (kind, peer, seq), (start, finished) in received.items():
        admitted = accepted[(kind, peer, seq)][0]
        require(0 < admitted <= finished <= closed[peer][0], "frame did not settle before EOF")
        last_seq, last_time = previous_by_peer.get((kind, peer), (-1, 0))
        require(seq == last_seq + 1 and finished >= last_time, "receive order mismatch")
        previous_by_peer[(kind, peer)] = (seq, finished)
        per_peer[peer] += 1
        if kind == "W":
            require(0 < start <= admitted <= finished <= begin, "warmup timing mismatch")
        elif kind == "B":
            require(start == 0 and begin <= admitted <= stop and release <= finished <= end,
                    "burst did not drain in the scheduled phase")
        else:
            require(begin <= start <= admitted <= finished <= end, "echo timing mismatch")
        if kind == measure_kind:
            latencies.append(finished - (admitted if slow else start))

    require([metrics["owner_cpu"], metrics["peer_cpu"]] == cpus
            and metrics["threads_during_work"] == 2 and metrics["threads_after"] == 1,
            "unexpected CPU placement or thread count")
    require(metrics["fd_after"] == metrics["fd_before"]
            and all(metrics[key] == 0 for key in COMMON_ZERO),
            "fd, connection, output, or EventLoop residue")
    require(metrics["connections_accepted"] == metrics["connections_retired"] == peer_count,
            "connection counts disagree")
    require(0 < metrics["owner_cpu_ns"] <= metrics["process_cpu_ns"]
            and 0 < metrics["rss_before_measure_kib"] <= metrics["rss_peak_kib"]
            and 0 < metrics["rss_after_drain_kib"] <= metrics["rss_peak_kib"]
            and 0 < metrics["rss_final_kib"] <= metrics["rss_peak_kib"],
            "invalid CPU time or RSS")
    native_ops = 0
    if backend == "io_uring":
        native_ops = check_native(directory, metrics, per_peer, peer_count)
    else:
        require(not (directory / "native-connections.csv").exists()
                and not any(key.startswith("native_") for key in metrics),
                "unexpected native evidence in readiness sample")

    latencies.sort()
    p99 = latencies[(99 * len(latencies) + 99) // 100 - 1]
    elapsed = end - begin
    measured_bytes = len(measured) * PROFILE["frame_bytes"] * (1 if slow else 2)
    return {
        "accepted_frames": len(accepted),
        "delivered_frames": len(received),
        "closed_peers": len(closed),
        "native_operations_retired": native_ops,
        "measure_elapsed_ms": elapsed / 1e6,
        "throughput_mib_s": measured_bytes * 1e9 / (2**20 * elapsed),
        "p99_us": p99 / 1e3,
        "cpu_ms": metrics["process_cpu_ns"] / 1e6,
        "cpu_pct_one_core": 100 * metrics["process_cpu_ns"] / elapsed,
        "owner_cpu_ms": metrics["owner_cpu_ns"] / 1e6,
        "peak_rss_mib": metrics["rss_peak_kib"] / 1024,
        "stop_ms": (done - stop) / 1e6,
        "pending_at_stop_bytes": metrics["slow_pending_at_stop_bytes"],
    }


def plan():
    for scenario in SCENARIOS:
        for pair in range(PAIRS + 1):
            order = BACKENDS if pair % 2 else BACKENDS[::-1]
            for backend in order:
                yield scenario, pair, backend


def trial_path(root, scenario, pair, backend):
    return root / "trials" / scenario / f"{pair:02d}-{backend}"


def replay(root, cpus):
    trials = []
    for scenario, pair, backend in plan():
        directory = trial_path(root, scenario, pair, backend)
        try:
            metrics = sample(directory, backend, scenario, cpus)
        except (ValueError, KeyError, OSError) as error:
            raise ValueError(f"{directory.relative_to(root)}: {error}") from error
        trials.append({"scenario": scenario, "backend": backend, "pair": pair, "metrics": metrics})

    groups = {}
    for scenario in SCENARIOS:
        groups[scenario] = {}
        for backend in BACKENDS:
            scored = [trial["metrics"] for trial in trials if trial["scenario"] == scenario
                      and trial["backend"] == backend and trial["pair"] > 0]
            groups[scenario][backend] = {
                "count": len(scored),
                "median": {key: statistics.median(row[key] for row in scored)
                           for key in scored[0]},
            }
    return {"groups": groups, "trials": trials}


def command(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def revision(directory):
    require(not command("git", "status", "--porcelain", cwd=directory),
            f"dirty Git checkout: {directory}")
    return command("git", "rev-parse", "HEAD", cwd=directory)


def native_host():
    kernel = platform.release()
    os_release = Path("/proc/sys/kernel/osrelease").read_text().strip()
    version = Path("/proc/version").read_text().strip()
    require(platform.system() == "Linux" and not re.search("microsoft|wsl", kernel + os_release + version, re.I)
            and not any(key in os.environ for key in ("WSL_INTEROP", "WSL_DISTRO_NAME")),
            "native Linux required; refusing WSL")
    allowed = sorted(os.sched_getaffinity(0))
    require(len(allowed) >= 2, "two CPUs required")
    topology = {}
    for number in allowed:
        cpu_dir = Path(f"/sys/devices/system/cpu/cpu{number}/topology")
        topology[number] = [(cpu_dir / field).read_text().strip()
                            for field in ("physical_package_id", "core_id")]
    first = allowed[0]
    second = next((n for n in allowed[1:] if topology[n] != topology[first]), allowed[1])

    return {
        "system": "Linux",
        "kernel": kernel,
        "proc_osrelease": os_release,
        "proc_version": version,
        "cpu_model": next(line.split(":", 1)[1].strip() for line in
                          Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
        "available_cpus": allowed,
        "pinned_cpus": [first, second],
        "cpu_topology": [topology[first], topology[second]],
        "tcp_wmem": Path("/proc/sys/net/ipv4/tcp_wmem").read_text().split(),
        "distribution": platform.freedesktop_os_release(),
        "compiler": command("c++", "--version").splitlines()[0],
        "cmake": command("cmake", "--version").splitlines()[0],
        "ci_run_id": os.getenv("GITHUB_RUN_ID"),
        "ci_run_attempt": os.getenv("GITHUB_RUN_ATTEMPT"),
    }


def cmake_cache(path):
    entries = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        entry = re.fullmatch(r"([^#/:][^:]*):[^=]+=(.*)", line)
        if entry:
            entries[entry[1]] = entry[2]
    return entries


def check_build(root, install_prefix):
    inputs = root / "inputs"
    core = cmake_cache(inputs / "package-cache.txt")
    consumer = cmake_cache(inputs / "consumer-cache.txt")
    for key, expected in CORE_CACHE_KEYS.items():
        require(core[key] == expected, f"unexpected package {key}={core[key]}")
    require(core["CMAKE_INSTALL_PREFIX"] == install_prefix
            and consumer["GameNetCore_DIR"] == install_prefix + "/lib/cmake/GameNetCore"
            and consumer["CMAKE_BUILD_TYPE"] == "Release"
            and all("-DNDEBUG" in config["CMAKE_CXX_FLAGS_RELEASE"]
                    for config in (core, consumer)), "Release or installed-package provenance mismatch")
    imported = (inputs / "installed-archives.txt").read_text(encoding="utf-8").splitlines()
    require(imported == [f"{install_prefix}/lib/{name}" for name in ARCHIVES],
            "consumer linked outside the installed archives")
    transcript = (root / "build/consumer-build.log").read_text(encoding="utf-8")
    require(f"{install_prefix}/include" in transcript and all(path in transcript for path in imported),
            "installed headers or archives missing in build log")
    for name in ARCHIVES:
        require((inputs / name).stat().st_size > 0, f"empty installed archive {name}")
    require((inputs / "installed_io_uring_acceptance").stat().st_size > 0, "missing consumer binary")


def verify(root):
    info = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    require(info["schema"] == SCHEMA and info["profile"] == PROFILE, "evidence schema or workload changed")
    require(info["files"] == file_manifest(root), "evidence inventory, size or SHA-256 mismatch")
    require(all(re.fullmatch("[0-9a-f]{40}", info[key]) for key in ("core_revision", "consumer_revision")),
            "invalid checkout provenance")
    host = info["host"]
    require(host["system"] == "Linux" and not any(re.search("microsoft|wsl", host[key], re.I)
            for key in ("kernel", "proc_osrelease", "proc_version")), "evidence is not from native Linux")
    cpus = host["pinned_cpus"]
    require(len(cpus) == 2 and cpus == sorted(set(cpus)) and set(cpus) <= set(host["available_cpus"]),
            "invalid CPU pairing")
    check_build(root, info["install_prefix"])
    trials = root / "trials"
    require({p.relative_to(trials).as_posix() for p in trials.glob("*/*") if p.is_dir()}
            == {trial_path(root, *entry).relative_to(trials).as_posix() for entry in plan()},
            "missing or unexpected trial")
    result = replay(root, cpus)
    require(result == json.loads((root / "summary.json").read_text(encoding="utf-8")),
            "summary differs from independently recomputed evidence")
    return result


def run(args):
    root = args.output.resolve()
    host = native_host()
    core = revision(args.source.resolve())
    consumer = revision(Path(__file__).resolve().parent)
    require(core == args.expected_source and consumer == os.getenv("GITHUB_SHA", consumer),
            "checkout is not the requested package or workflow revision")
    package_build = args.package_build.resolve()
    consumer_build = args.consumer_build.resolve()
    prefix = args.prefix.resolve()
    binary = consumer_build / "installed_io_uring_acceptance"
    inputs = root / "inputs"
    inputs.mkdir(parents=True)
    copies = {
        package_build / "CMakeCache.txt": "package-cache.txt",
        consumer_build / "CMakeCache.txt": "consumer-cache.txt",
        consumer_build / "installed-archives.txt": "installed-archives.txt",
        binary: binary.name,
    }
    copies.update({prefix / "lib" / name: name for name in ARCHIVES})
    for source, target in copies.items():
        shutil.copyfile(source, inputs / target)
    check_build(root, str(prefix))
    os.sched_setaffinity(0, host["pinned_cpus"])
    for scenario, pair, backend in plan():
        directory = trial_path(root, scenario, pair, backend)
        directory.mkdir(parents=True)
        print(f"{scenario} pair {pair}: {backend}", flush=True)
        log_path = directory / "trial.log"
        try:
            with log_path.open("wb") as log:
                subprocess.run([binary, backend, scenario, directory], stdout=log,
                               stderr=subprocess.STDOUT, check=True, timeout=45)
        except subprocess.SubprocessError:
            print(log_path.read_bytes()[-4096:].decode(errors="replace"), file=sys.stderr)
            raise
        sample(directory, backend, scenario, host["pinned_cpus"])

    result = replay(root, host["pinned_cpus"])
    write_json(root / "summary.json", result)
    write_json(root / "manifest.json", {
        "schema": SCHEMA, "profile": PROFILE, "host": host, "core_revision": core,
        "consumer_revision": consumer, "install_prefix": str(prefix), "files": file_manifest(root),
    })
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-output", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--expected-source")
    parser.add_argument("--package-build", type=Path)
    parser.add_argument("--consumer-build", type=Path)
    parser.add_argument("--prefix", type=Path)
    args = parser.parse_args()
    if args.verify_output:
        result = verify(args.verify_output.resolve())
        print("Independent replay: all trial ledgers, package provenance, and cleanup verified")
    else:
        if not all((args.output, args.source, args.expected_source, args.package_build,
                    args.consumer_build, args.prefix)):
            parser.error("run mode requires source, expected source, package and consumer builds, prefix and output")
        result = run(args)
    print(json.dumps(result["groups"], indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
