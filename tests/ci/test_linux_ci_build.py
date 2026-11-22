# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


DRIVER = Path(__file__).resolve().parents[2] / "tools/run_linux_ci_build.py"
EVIDENCE = Path("ci-evidence/linux-build")
FAKE_CMAKE = """#!/usr/bin/env python3
import subprocess
import sys

mode = sys.argv[2].removeprefix("build-")
assert sys.argv[1:] == ["--build", "build-" + mode, "--parallel", "2", "--target", "fixture"]
print("compiler output", flush=True)
print("compiler diagnostic", file=sys.stderr, flush=True)
if mode == "error":
    sys.exit(17)
if mode == "timeout":
    child = subprocess.Popen([sys.executable, "-c",
        "import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
        "print('child ready', flush=True); time.sleep(60)"])
    print("child_pid=" + str(child.pid), flush=True)
    child.wait()
"""


def check_build(root, env, mode, expected_exit):
    result = subprocess.run(
        [sys.executable, str(DRIVER), "build", f"build-{mode}",
         "--target", "fixture", "--deadline-seconds", "2"],
        cwd=root, env=env, capture_output=True, text=True, timeout=20,
    )
    assert result.returncode == expected_exit, result.stdout + result.stderr
    raw = (root / EVIDENCE / f"build-{mode}.log").read_text(encoding="utf-8")
    assert raw == result.stdout
    assert "compiler output\n" in raw and "compiler diagnostic\n" in raw
    records = [json.loads(line.removeprefix("CI_BUILD ")) for line in raw.splitlines()
               if line.startswith("CI_BUILD ")]
    start, finish = records[0], records[-1]
    assert start["event"] == "start" and start["deadline_seconds"] == 2
    assert start["command"] == ["cmake", "--build", f"build-{mode}", "--parallel", "2", "--target", "fixture"]
    assert finish["event"] == "finish" and finish["exit_code"] == expected_exit
    assert all(record["cpu_affinity_count"] > 0 and record["memory_kib"]["MemTotal"] > 0
               for record in records)
    if mode == "timeout":
        assert "sending signal TERM" in raw and "child ready\n" in raw
        assert 2 <= finish["elapsed_seconds"] < 10, finish
        child_pid = next(line.split("=", 1)[1] for line in raw.splitlines() if line.startswith("child_pid="))
        status = Path("/proc") / child_pid / "stat"
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                state = status.read_text(encoding="utf-8").rsplit(")", 1)[1].split()[0]
            except FileNotFoundError:
                break
            if state == "Z":
                break
            time.sleep(0.02)
        else:
            raise AssertionError("timed-out build left a running child")


def main():
    with tempfile.TemporaryDirectory(prefix="gamenet-linux-build-") as temporary:
        root = Path(temporary)
        command = root / "cmake"
        command.write_text(FAKE_CMAKE, encoding="utf-8")
        command.chmod(0o755)
        env = dict(os.environ, LC_ALL="C", PATH=str(root) + os.pathsep + os.environ["PATH"])
        subprocess.run([sys.executable, str(DRIVER), "preflight"], cwd=root, env=env,
                       check=True, stdout=subprocess.DEVNULL, timeout=10)
        preflight = json.loads((root / EVIDENCE / "preflight.json").read_text(encoding="utf-8"))
        assert preflight["event"] == "preflight" and preflight["build_parallelism"] == 2
        assert preflight["deadline_seconds"] == 600 and preflight["memory_kib"]["MemTotal"] > 0
        for mode, code in (("success", 0), ("error", 17), ("timeout", 124)):
            check_build(root, env, mode, code)
    print("Linux CI build deadline, child cleanup and diagnostic retention verified")


if __name__ == "__main__":
    main()
