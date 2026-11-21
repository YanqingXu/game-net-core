# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time


REPO = Path(__file__).resolve().parents[1]
BENCHMARK = REPO / "benchmarks/kcp_gateway_replay"
PROFILE = "gateway-q3-q5.json"
SCHEMA = "gamenet.kcp_gateway_replay.v1"
SOURCE_FILES = tuple((BENCHMARK / name).relative_to(REPO).as_posix()
                     for name in ("CMakeLists.txt", "ReplayProfile.h.in", "replay.cpp", PROFILE)) + (
    "tools/run_kcp_gateway_replay.py",
)
MASK = dict(warmup=3, fanout=3, oversize=3, pressure=2, probe=2)
FILES = ("offers.csv", "admission.csv", "terminals.csv", "receive.csv", "faults.csv", "final.csv", "profile.sha256")
SIDE_BITS = {"healthy": 1, "slow": 2}
CI_FIELDS = {"workflow_sha": "GITHUB_SHA", "run_id": "GITHUB_RUN_ID", "run_attempt": "GITHUB_RUN_ATTEMPT"}


def require(valid, why):
    if not valid:
        raise ValueError(why)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def source_bytes(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def git(*args):
    return subprocess.check_output(
        ["git", "-c", "core.autocrlf=true", "-c", "core.safecrlf=false", *args], cwd=REPO
    ).decode().strip()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def input_snapshot():
    return {
        "commit": git("rev-parse", "HEAD"),
        "tree": git("rev-parse", "HEAD^{tree}"),
        "tracked_changes": bool(git("status", "--porcelain", "--untracked-files=no")),
        "files": {name: sha(source_bytes(REPO / name)) for name in SOURCE_FILES},
    }


def hex_id(value, length):
    return isinstance(value, str) and re.fullmatch(f"[0-9a-f]{{{length}}}", value) is not None


def read_profile(path):
    profile = read_json(path)
    require(profile["schema"] == "gamenet.kcp_gateway_profile.v1", "unknown workload schema")
    source = profile["provenance"]
    require(source["consumer"] == "gamenet-game-gateway" and
            all(hex_id(source[key], 40) for key in ("consumer_commit", "evidence_commit")) and
            len(source["source_sha256"]) == 4 and
            all(hex_id(value, 64) for value in source["source_sha256"].values()), "gateway source identity")
    require(type(source["replay_cycles"]) is int and source["replay_cycles"] > 0 and
            type(source["replay_elapsed_us"]) is int and source["replay_elapsed_us"] > 0, "gateway replay cadence")
    work = profile["workload"]
    bodies = ("warmup_body", "broadcast_body", "recovery_body")
    require(all(re.fullmatch("[A-Za-z0-9:._-]+", work[key]) for key in bodies) and
            len({work[key] for key in bodies}) == len(bodies), "profile message bodies")
    require(all(type(value) is int and 0 < value <= 60_000_000
                for key, value in work.items() if key not in bodies), "profile numbers")
    require(work["epoch_us"] == round(source["replay_elapsed_us"] / source["replay_cycles"]) and
            0 < work["first_broadcast_us"] <= work["broadcast_interval_us"] and
            work["oversize_at_us"] < work["fault_begin_us"] < work["pressure_at_us"] <
            work["fault_restore_us"] < work["epoch_us"], "profile schedule")
    require(work["broadcast_recipients"] == 2 and work["slow_recipients"] == 1 and
            work["router_fanout_limit"] >= work["broadcast_recipients"], "fixture topology")
    maximum = max(work["measure_epochs"], work["smoke_epochs"])
    healthy_bound = maximum * work["epoch_us"] // work["broadcast_interval_us"] + 1 + work["warmup_broadcasts"] + maximum
    require(work["smoke_epochs"] < work["measure_epochs"] < 255 and work["measure_repetitions"] <= 10 and
            healthy_bound <= 2000 and healthy_bound + maximum * (work["pressure_messages"] + 1) <= 20000,
            "profile evidence bounds")
    return profile


def read_rows(directory, name):
    with (directory / name).open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            require(None not in row and all(value is not None for value in row.values()), name)
            yield {key: value if key in {"side", "kind", "status", "result"} else int(value)
                   for key, value in row.items()}


def index(rows, *keys):
    result = {}
    for row in rows:
        key = tuple(row[field] for field in keys)
        require(key not in result, f"duplicate {keys} {key}")
        result[key] = row
    return result


def quantiles(values):
    require(values, "no observations for percentile")
    data = sorted(values)
    return dict(n=len(data), min_us=data[0], p50_us=data[math.ceil(0.5 * len(data)) - 1],
                p99_us=data[math.ceil(0.99 * len(data)) - 1], max_us=data[-1])


def analyze(directory, epochs, work):
    final, = read_rows(directory, "final.csv")
    offers = index(read_rows(directory, "offers.csv"), "seq")
    tx = index(read_rows(directory, "admission.csv"), "side", "seq")
    completions = index(read_rows(directory, "terminals.csv"), "side", "send_id")
    rx = index(read_rows(directory, "receive.csv"), "side", "seq")
    faults = list(read_rows(directory, "faults.csv"))
    start = final["start_us"]
    epoch = work["epoch_us"]
    tick = work["broadcast_interval_us"]
    recipients_per_broadcast = work["broadcast_recipients"]
    end = epochs * epoch
    require(final["epoch_us"] == epoch and final["epochs"] == epochs and start > 0, "protocol workload drift")
    require(final["residual"] == 0 and final["relay_refusals"] == 0, "fixture socket or resource settlement")
    require([final[field] for field in ("healthy_opened", "slow_opened", "healthy_client_opened",
                                      "slow_client_opened")] == [1, epochs + 1, 1, epochs + 1],
            "native generation opened counts")
    require(list(offers) == [(n,) for n in range(1, len(offers) + 1)], "offer sequence")
    planned = {
        "warmup": list(range(-work["warmup_broadcasts"] * tick, 0, tick)),
        "fanout": list(range(work["first_broadcast_us"], end, tick)),
        "oversize": [i * epoch + work["oversize_at_us"] for i in range(epochs)],
        "pressure": [i * epoch + work["pressure_at_us"] for i in range(epochs)
                     for _ in range(work["pressure_messages"])],
        "probe": [i * epoch + work["fault_restore_us"] for i in range(epochs)],
    }
    suffix_bytes = {
        "warmup": len("BROADCAST  ") + len(work["warmup_body"]),
        "fanout": len("BROADCAST  ") + len(work["broadcast_body"]),
        "oversize": len("BROADCAST  ") + work["oversize_body_bytes"],
        "pressure": len("RESULT  ") + work["pressure_body_bytes"],
        "probe": len("RESULT  ") + len(work["recovery_body"]),
    }
    actual = {kind: [] for kind in MASK}
    recipients = {}
    for o in offers.values():
        seq, kind = o["seq"], o["kind"]
        actual[kind].append(o["scheduled_us"] - start)
        require(o["published_us"] >= o["scheduled_us"] and o["mask"] == MASK[kind] and
                o["bytes"] == suffix_bytes[kind] + len(str(seq)), f"offer content {seq}")
        accepted = 0
        for side, bit in SIDE_BITS.items():
            if not (o["mask"] & bit):
                continue
            key = (side, seq)
            admission = tx.get(key)
            require(kind != "oversize" or admission is None, "policy probe reached transport")
            accepted += bool(admission and admission["result"] == "accepted")
            recipients[key] = o
        if kind == "pressure":
            require(("slow", seq) in tx, "absent offered pressure send")
            require(not (o["dispatch_accepted"] or o["dispatch_dropped"] or o["byte_limit"]), "unicast route")
        else:
            require(o["dispatch_accepted"] == accepted and o["dispatch_dropped"] >= 0 and
                    o["dispatch_accepted"] + o["dispatch_dropped"] == o["mask"].bit_count(),
                    "broadcast/transport accounting")
            require(o["byte_limit"] == (recipients_per_broadcast if kind == "oversize" else 0), "oversize policy probe")
    require(actual == planned, "open-loop schedule coverage")
    require(set(tx) <= set(recipients), "unsolicited admission")

    accepted_keys = set()
    for key, txrow in tx.items():
        o = recipients[key]
        require(txrow["at_us"] >= o["published_us"] and txrow["bytes"] == o["bytes"], "admission alignment")
        if key[0] == "healthy":
            require(txrow["conv"] == 101, "healthy generation changed")
        if o["kind"] == "pressure":
            require(txrow["conv"] == 200 + (o["scheduled_us"] - start) // epoch, "pressure used the wrong generation")
        if txrow["result"] == "accepted":
            k = (key[0], txrow["send_id"])
            require(k not in accepted_keys and txrow["send_id"] > 0 and k in completions, "send terminal key")
            stop = completions[k]
            require(stop["conv"] == txrow["conv"] and stop["at_us"] >= txrow["at_us"] and
                    stop["bytes"] == txrow["bytes"] and stop["status"] in {"acknowledged", "cancelled", "failed"},
                    "typed terminal alignment")
            accepted_keys.add(k)
        else:
            require(txrow["send_id"] == 0 and txrow["result"] in
                    {"closed", "overloaded", "no_memory", "owner_unavailable"}, "rejected send id/status")
    require(set(completions) == accepted_keys, "missing/duplicate/unsolicited terminal")
    for side in SIDE_BITS:
        require(sum(k[0] == side for k in accepted_keys) == final[f"{side}_accepted"], "KCP accepted snapshot")

    for key, row in rx.items():
        require(key in tx and tx[key]["result"] == "accepted", "delivery without acceptance")
        o = recipients[key]
        require(row["kind"] == o["kind"] and row["bytes"] == o["bytes"] and
                row["conv"] == tx[key]["conv"] and row["at_us"] >= tx[key]["at_us"], "wrong payload/generation")

    recovery, fault_to_receive, rebind, first_receive, fault_dwell = [], [], [], [], []
    require(len(faults) == epochs and sum(f["wire_drops"] for f in faults) == final["relay_wire_drops"],
            "native fault coverage")
    for i, f in enumerate(faults):
        due = start + i * epoch
        require(f["cycle"] == i + 1 and f["start_due_us"] == due + work["fault_begin_us"] and
                f["stop_due_us"] == due + work["fault_restore_us"] and f["old_conv"] == 200 + i and
                f["new_conv"] == f["old_conv"] + 1 and f["wire_drops"] > 0, "fault sequence")
        require(f["start_us"] >= f["start_due_us"] and f["restored_us"] >= f["stop_due_us"] and
                f["rebind_us"] >= f["restored_us"] >= f["start_us"], "fault clocks")
        o = offers[(f["probe_id"],)]
        row = rx[("slow", f["probe_id"])]
        require(o["kind"] == "probe" and o["scheduled_us"] == f["stop_due_us"] and
                o["published_us"] >= f["rebind_us"] and row["conv"] == f["new_conv"], "fresh recovery receive")
        recovery.append(row["at_us"] - f["restored_us"])
        fault_to_receive.append(row["at_us"] - f["start_us"])
        rebind.append(f["rebind_us"] - f["restored_us"])
        first_receive.append(row["at_us"] - f["rebind_us"])
        fault_dwell.append(f["restored_us"] - f["start_us"])
    pressure_by_epoch = Counter((recipients[key]["scheduled_us"] - start) // epoch
                                for key, val in tx.items()
                                if recipients[key]["kind"] == "pressure" and val["result"] == "overloaded")
    require(len(pressure_by_epoch) == epochs, "missing real overload in a fault epoch")

    counters = {}
    latency = {k: [] for k in ("healthy_publish", "healthy_scheduled", "healthy_publish_fault",
                               "healthy_publish_other")}
    for key, o in recipients.items():
        kind, side = o["kind"], key[0]
        if kind == "warmup":
            continue
        bucket = counters.setdefault(f"{side}_{kind}", Counter())
        bucket["offered"] += 1
        admit = tx.get(key)
        row = rx.get(key)
        if admit and admit["result"] == "accepted":
            bucket["accepted"] += 1
            bucket[completions[(side, admit["send_id"])]["status"]] += 1
            bucket["received" if row else "no_receive"] += 1
        else:
            bucket["rejected"] += 1
            if admit:
                reason = admit["result"]
            else:
                reason = "task_byte_limit" if kind == "oversize" else "before_kcp"
            bucket[f"reject_{reason}"] += 1
        if row and side == "healthy" and kind == "fanout":
            elapsed = row["at_us"] - o["published_us"]
            latency["healthy_publish"].append(elapsed)
            latency["healthy_scheduled"].append(row["at_us"] - o["scheduled_us"])
            offset = (o["scheduled_us"] - start) % epoch
            phase = "fault" if work["fault_begin_us"] <= offset < work["fault_restore_us"] else "other"
            latency[f"healthy_publish_{phase}"].append(elapsed)
    latency.update(recovery=recovery, fault_to_receive=fault_to_receive, rebind=rebind,
                   rebind_to_receive=first_receive, fault_dwell=fault_dwell)
    return dict(epochs=epochs, duration_us=end, wire_drops=final["relay_wire_drops"], counters=counters,
                samples=latency, metrics={key: quantiles(value) for key, value in latency.items()})


def package_files(prefix):
    files = {p.relative_to(prefix).as_posix(): digest(p) for p in sorted(prefix.rglob("*")) if p.is_file()}
    require(any(Path(name).suffix in {".lib", ".a"} and "gamenet_kcp" in Path(name).name for name in files),
            "missing installed KCP library")
    return files


def build_replay(cmake, prefix, build):
    subprocess.run([
        cmake, "-S", str(BENCHMARK), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
    ], check=True)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    package = re.search(r"^GameNetCore_DIR:PATH=(.+)$", cache, re.MULTILINE)
    require(package and Path(package[1]).resolve().is_relative_to(prefix),
            "CMake did not select the requested GameNet prefix")
    subprocess.run([cmake, "--build", str(build), "--config", "Release", "--parallel", "2"], check=True)
    binary, compiler = (build / "gateway-replay-Release.txt").read_text(encoding="utf-8").splitlines()
    binary = Path(binary).resolve()
    require(binary.is_relative_to(build) and binary.is_file() and compiler, "invalid Release launcher metadata")
    return binary, compiler


def sample_plan(mode, work):
    require(mode in {"smoke", "measure"}, "unknown sample mode")
    if mode == "smoke":
        return 1, work["smoke_epochs"]
    return work["measure_repetitions"], work["measure_epochs"]


def ci_identity():
    return {key: os.getenv(env) for key, env in CI_FIELDS.items()}


def run(args):
    prefix = args.package_prefix.resolve()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    require(prefix.is_dir(), "installed Release package prefix required")
    require(not build.is_relative_to(prefix) and not output.is_relative_to(prefix),
            "build and evidence directories must be outside the package prefix")
    profile_path = BENCHMARK / PROFILE
    work = read_profile(profile_path)["workload"]
    repetitions, epochs = sample_plan(args.mode, work)
    backend = {"Windows": "iocp", "Linux": "epoll"}.get(platform.system())
    require(backend, "gateway replay supports Windows and Linux")
    source = input_snapshot()
    ci = ci_identity()
    if ci["workflow_sha"] is not None:
        require(ci["workflow_sha"] == source["commit"] and not source["tracked_changes"],
                "CI replay source does not match the clean workflow checkout")
    profile_bytes = source_bytes(profile_path)
    profile_hash = sha(profile_bytes)
    output.mkdir(parents=True, exist_ok=False)
    (output / PROFILE).write_bytes(profile_bytes)
    install_files = package_files(prefix)
    binary, compiler = build_replay(args.cmake, prefix, build)
    binary_hash = digest(binary)
    provenance = dict(schema=SCHEMA, mode=args.mode, configuration="Release", source=source, ci=ci,
                      platform=platform.platform(), backend=backend, wsl=os.getenv("WSL_DISTRO_NAME"),
                      cpu_count=os.cpu_count(), compiler=compiler, profile_sha256=profile_hash,
                      binary_sha256=binary_hash, package_files=install_files)
    write_json(output / "provenance.json", provenance)
    if backend == "epoll":
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    duration_us = epochs * work["epoch_us"]
    for rep in range(1, repetitions + 1):
        evidence = output / f"run-{rep:02d}"
        command = [str(binary), str(epochs), evidence.name]
        log = output / f"run-{rep:02d}.log"
        began = datetime.now(timezone.utc).isoformat()
        start = time.monotonic_ns()
        with log.open("wb") as stream:
            with subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, cwd=output) as child:
                print(f"{backend} Release run {rep}/{repetitions}, PID {child.pid}", flush=True)
                try:
                    code = child.wait(timeout=duration_us / 1_000_000 + 30)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
                    raise
                pid = child.pid
        wall_us = (time.monotonic_ns() - start) // 1000
        require(code == 0 and wall_us >= duration_us, f"sample did not complete: {log}")
        require((evidence / "profile.sha256").read_text(encoding="ascii").strip() == profile_hash,
                "compiled profile differs from archived profile")
        require(digest(binary) == binary_hash, "binary changed while sampling")
        manifest = dict(repetition=rep, epochs=epochs, binary_sha256=binary_hash, pid=pid, returncode=code,
                        start_utc=began, finish_utc=datetime.now(timezone.utc).isoformat(),
                        wall_us=wall_us, files={f: digest(evidence / f) for f in FILES},
                        log_sha256=digest(log))
        write_json(evidence / "manifest.json", manifest)
        print(f"{backend} Release run {rep}/{repetitions} completed", flush=True)
    require(input_snapshot() == source and package_files(prefix) == install_files,
            "source or installed package changed during sampling")
    write_json(output / "summary.json", readback(output))
    verify_output(output)


def readback(output):
    provenance = read_json(output / "provenance.json")
    require(provenance["schema"] == SCHEMA and provenance["configuration"] == "Release" and
            provenance["backend"] in {"iocp", "epoll"} and provenance["compiler"] and
            hex_id(provenance["binary_sha256"], 64), "invalid Release replay provenance")
    profile = read_profile(output / PROFILE)
    work = profile["workload"]
    repetitions, epochs = sample_plan(provenance["mode"], work)
    source = provenance["source"]
    profile_hash = digest(output / PROFILE)
    require(hex_id(source["commit"], 40) and hex_id(source["tree"], 40) and
            set(source["files"]) == set(SOURCE_FILES) and
            all(hex_id(value, 64) for value in source["files"].values()) and
            source["files"][(BENCHMARK / PROFILE).relative_to(REPO).as_posix()] ==
            provenance["profile_sha256"] == profile_hash, "source/profile evidence identity")
    require(provenance["package_files"] and all(hex_id(value, 64) for value in provenance["package_files"].values()),
            "invalid installed package identity")
    ci = provenance["ci"]
    if ci["workflow_sha"] is not None:
        require(ci["workflow_sha"] == source["commit"] and source["tracked_changes"] is False and
                all(isinstance(ci[key], str) and ci[key].isdigit() and int(ci[key]) > 0
                    for key in ("run_id", "run_attempt")), "invalid CI sample identity")
    for key, expected in ci_identity().items():
        if expected is not None:
            require(ci[key] == expected, f"evidence does not match this CI job: {key}")
    expected_dirs = {f"run-{i:02d}" for i in range(1, repetitions + 1)}
    require({p.name for p in output.iterdir() if p.is_dir()} == expected_dirs, "unexpected sample inventory")
    pooled = {}
    totals = {}
    per_run = []
    total_wire_drops = 0
    for i in range(1, repetitions + 1):
        path = output / f"run-{i:02d}"
        manifest = read_json(path / "manifest.json")
        require(manifest["returncode"] == 0 and manifest["pid"] > 0 and
                manifest["wall_us"] >= epochs * work["epoch_us"] and manifest["epochs"] == epochs and
                manifest["repetition"] == i and manifest["binary_sha256"] == provenance["binary_sha256"],
                "sample execution")
        require(manifest["files"] == {f: digest(path / f) for f in FILES} and
                manifest["log_sha256"] == digest(output / f"run-{i:02d}.log"), "raw output hash mismatch")
        require((path / "profile.sha256").read_text(encoding="ascii").strip() == profile_hash,
                "compiled profile differs from archived profile")
        dataset = analyze(path, epochs, work)
        for key, values in dataset.pop("samples").items():
            pooled.setdefault(key, []).extend(values)
        for key, counter in dataset["counters"].items():
            totals.setdefault(key, Counter()).update(counter)
        total_wire_drops += dataset["wire_drops"]
        per_run.append(dict(repetition=i, pid=manifest["pid"], **dataset))
    return dict(schema=SCHEMA, mode=provenance["mode"], configuration=provenance["configuration"],
                platform=provenance["platform"], backend=provenance["backend"], wsl=provenance["wsl"],
                compiler=provenance["compiler"], source=source, profile_sha256=profile_hash,
                consumer=profile["provenance"], samples=per_run, wire_drops=total_wire_drops,
                metrics={key: quantiles(values) for key, values in pooled.items()}, counters=totals,
                rejection_rates={key: counter["rejected"] / counter["offered"] for key, counter in totals.items()})


def verify_output(output):
    summary = readback(output)
    require(read_json(output / "summary.json") == summary, "summary differs from raw evidence")
    healthy = summary["counters"]["healthy_fanout"]
    recovery = summary["metrics"]["recovery"]
    print(json.dumps(dict(
        configuration=summary["configuration"], backend=summary["backend"], mode=summary["mode"],
        commit=summary["source"]["commit"], profile_sha256=summary["profile_sha256"],
        healthy_received=healthy["received"], healthy_offered=healthy["offered"],
        healthy_rejected=healthy["rejected"], healthy_p99_us=summary["metrics"]["healthy_publish"]["p99_us"],
        recovery_probes=recovery["n"], recovery_p50_us=recovery["p50_us"], recovery_p99_us=recovery["p99_us"],
    ), indent=2))


def main():
    parser = argparse.ArgumentParser(description="Run or independently verify the Release KCP gateway replay")
    commands = parser.add_mutually_exclusive_group(required=True)
    commands.add_argument("--package-prefix", type=Path, help="installed GameNet Release prefix")
    commands.add_argument("--verify-output", type=Path, help="evidence directory to verify without building")
    parser.add_argument("--build-dir", type=Path, help="standalone benchmark build directory")
    parser.add_argument("--output", type=Path, help="new evidence directory")
    parser.add_argument("--mode", choices=("measure", "smoke"), default="measure")
    parser.add_argument("--cmake", default="cmake", help="CMake executable")
    args = parser.parse_args()
    if args.verify_output is not None:
        verify_output(args.verify_output)
    else:
        if args.build_dir is None or args.output is None:
            parser.error("--package-prefix requires --build-dir and --output")
        run(args)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(f"KCP gateway replay: {exc}")
