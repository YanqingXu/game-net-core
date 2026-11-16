# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Run both actual KCP-C2 echo processes and reject incomplete/false-green evidence."""
from __future__ import annotations
import argparse
import hashlib
import json
import queue
import re
import socket
import subprocess
import threading
from pathlib import Path

MESSAGES = {"empty": "", "fragmented": "c2:" + "x" * 4096}
PAIRS = {f"ipv{family}-{kind}": (family, text) for family in (4, 6) for kind, text in MESSAGES.items()}
NEGATIVES = {"server-usage": ("server", 2, "Usage:"),
             "client-conv": ("client", 2, "KCP_ECHO_ERROR:"),
             "client-port": ("client", 2, "KCP_ECHO_ERROR:"),
             "server-ip": ("server", 2, "KCP_ECHO_ERROR:"),
             "client-deadline": ("client", 1, "KCP_ECHO_DEADLINE:")}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def summary(log, role, passed, size):
    require(not re.search(r"AddressSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:|DEADLYSIGNAL", log),
            "example sanitizer diagnostic")
    rows = [line.split() for line in log.splitlines() if line.startswith("KCP_ECHO_SUMMARY ")]
    require(len(rows) == 1 and len(rows[0]) == 10, "missing/duplicate example summary")
    row = rows[0]
    require(row[1:3] == [role, "passed" if passed else "failed"], "wrong example outcome")
    expected = [1, 1, 1, 0, 0, 1, size] if passed else [0, 1, 0, 1, 0, 1, 0]
    require(list(map(int, row[3:])) == expected, "wrong example settlement or payload size")


def verify_examples(output):
    output = Path(output)
    result = json.loads((output / "results.json").read_text(encoding="utf-8"))
    require(result.get("schema") == "gamenet.kcp_examples.v1" and result.get("status") == "passed", "incomplete examples")
    require(set(result.get("cases", {})) == PAIRS.keys() | NEGATIVES.keys(), "example case inventory drift")
    require(set(result.get("binaries", {})) == {"server", "client"} and
            all(re.fullmatch(r"[0-9a-f]{64}", value) for value in result["binaries"].values()), "invalid example binary identities")
    for name, (_, text) in PAIRS.items():
        require(result["cases"][name] == {"server": 0, "client": 0}, "example process failed")
        for role in ("server", "client"):
            summary((output / name / (role + ".log")).read_text(encoding="utf-8"), role, True, len(text.encode()))
    for name, (role, code, marker) in NEGATIVES.items():
        require(result["cases"][name] == {role: code}, "negative example exited for wrong reason")
        log = (output / name / (role + ".log")).read_text(encoding="utf-8")
        require(marker in log, "negative example diagnostic missing")
        if name == "client-deadline":
            summary(log, "client", False, 0)
    return result


def exercise_examples(server, client, output, env):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    require(not any(output.iterdir()), "use fresh example evidence")
    binaries = {"server": str(Path(server).resolve()), "client": str(Path(client).resolve())}
    result = {"schema": "gamenet.kcp_examples.v1", "status": "failed", "cases": {},
              "binaries": {role: hashlib.sha256(Path(path).read_bytes()).hexdigest() for role, path in binaries.items()}}

    def command(directory, role, argv):
        value = [binaries[role], *argv]
        (directory / (role + "-command.json")).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        return value

    def execute(directory, role, argv):
        with (directory / (role + ".log")).open("wb") as log:
            run = subprocess.run(command(directory, role, argv), env=env, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=15)
        return run.returncode

    try:
        for index, (name, (family, text)) in enumerate(PAIRS.items()):
            directory = output / name
            directory.mkdir()
            host = "::1" if family == 6 else "127.0.0.1"
            native = socket.AF_INET6 if family == 6 else socket.AF_INET
            # Reserve distinct native ports until immediately before the actual processes bind.
            with socket.socket(native, socket.SOCK_DGRAM) as a, socket.socket(native, socket.SOCK_DGRAM) as b:
                a.bind((host, 0)); b.bind((host, 0))
                left, right = str(a.getsockname()[1]), str(b.getsockname()[1])
            conv = str(1000 + index)
            ready = queue.Queue()
            log = []
            process = subprocess.Popen(command(directory, "server", [host, left, host, right, conv, "10000"]),
                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")

            def reader():
                try:
                    for line in process.stdout:
                        log.append(line)
                        if line.startswith("KCP_ECHO_READY "):
                            ready.put(True)
                finally:
                    ready.put(False)

            thread = threading.Thread(target=reader, daemon=True)
            thread.start()
            try:
                require(ready.get(timeout=10), "server failed before readiness")
                code = execute(directory, "client", [host, right, host, left, conv, text, "2000"])
                result["cases"][name] = {"client": code, "server": process.wait(timeout=12)}
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                thread.join(timeout=5)
                require(not thread.is_alive(), "server log reader did not retire")
                process.stdout.close()
                (directory / "server.log").write_text("".join(log), encoding="utf-8")
        for name, (role, _, _) in NEGATIVES.items():
            directory = output / name
            directory.mkdir()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent_peer:
                silent_peer.bind(("127.0.0.1", 0))
                argv = ["127.0.0.1", "0", "127.0.0.1", str(silent_peer.getsockname()[1]), "2001"]
                if name == "server-usage": argv = []
                elif name == "client-conv": argv[4] = "0"
                elif name == "client-port": argv[1] = "65536"
                elif name == "server-ip": argv[0] = "invalid-ip"
                else: argv += ["deadline", "100"]
                result["cases"][name] = {role: execute(directory, role, argv)}
        result["status"] = "passed"
    finally:
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    try:
        verify_examples(output)
    except Exception:
        result["status"] = "failed"
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        raise
    return result


def self_test():
    good = "KCP_ECHO_SUMMARY client passed 1 1 1 0 0 1 4099\n"
    summary(good, "client", True, 4099)
    for bad in ["", good + good, good.replace(" 4099", " 0"), good.replace(" 1 4099", " 0 4099"),
                good.replace("passed", "failed"), good + "WARNING: ThreadSanitizer: data race"]:
        try:
            summary(bad, "client", True, 4099)
        except ValueError:
            pass
        else:
            raise AssertionError("false-green example summary accepted")
    summary("KCP_ECHO_SUMMARY client failed 0 1 0 1 0 1 0\n", "client", False, 0)
    print("KCP-C2 example payload/settlement/diagnostic rejection checks passed")


if __name__ == "__main__":
    import os
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--server", type=Path); parser.add_argument("--client", type=Path)
    parser.add_argument("--output", type=Path); parser.add_argument("--verify-output", type=Path)
    args = parser.parse_args()
    if args.self_test: self_test()
    elif args.verify_output: verify_examples(args.verify_output); print("KCP-C2 example evidence readback passed")
    elif args.server and args.client and args.output:
        exercise_examples(args.server, args.client, args.output, os.environ.copy())
        print("KCP-C2 examples passed: four dual-family exchanges and five negative cases")
    else: parser.error("self-test, verify-output, or server/client/output required")
