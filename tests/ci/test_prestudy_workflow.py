# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import copy
import json
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

from run_prestudy_contracts import (
    CONTRACTS,
    configure_command,
    verify_inventory,
    verify_results,
)


def rejects(callback, value) -> None:
    try:
        callback(value)
    except ValueError:
        return
    raise AssertionError("invalid prestudy evidence was accepted")


def test_evidence_validation() -> None:
    inventory = {
        "tests": [
            {
                "name": name,
                "properties": [{
                    "name": "LABELS", "value": ["contract", "experimental", "prestudy"],
                }],
            }
            for name in CONTRACTS
        ]
    }
    verify_inventory(inventory)
    rejects(verify_inventory, {"tests": []})
    for names in (
        list(CONTRACTS)[:-1],
        [*list(CONTRACTS)[:-1], next(iter(CONTRACTS))],
        [*list(CONTRACTS)[:-1], "contract.unrelated.test_replacement"],
        [*CONTRACTS, "contract.unrelated.test_extra"],
    ):
        changed = copy.deepcopy(inventory)
        changed["tests"] = [dict(inventory["tests"][0], name=name) for name in names]
        rejects(verify_inventory, changed)
    changed = copy.deepcopy(inventory)
    changed["tests"][0]["properties"][0]["value"].remove("prestudy")
    rejects(verify_inventory, changed)
    changed = copy.deepcopy(inventory)
    changed["tests"][0]["properties"].append({"name": "DISABLED", "value": True})
    rejects(verify_inventory, changed)

    results = ET.Element("testsuite", tests="7", failures="0", disabled="0", skipped="0")
    for name in CONTRACTS:
        ET.SubElement(results, "testcase", name=name, status="run")
    verify_results(results)
    rejects(verify_results, ET.Element("testsuite"))
    for tag in ("failure", "error", "skipped"):
        changed_results = copy.deepcopy(results)
        ET.SubElement(changed_results[0], tag)
        rejects(verify_results, changed_results)
    changed_results = copy.deepcopy(results)
    changed_results[0].set("status", "notrun")
    rejects(verify_results, changed_results)
    changed_results = copy.deepcopy(results)
    changed_results[0].set("name", results[1].get("name"))
    rejects(verify_results, changed_results)
    changed_results = copy.deepcopy(results)
    changed_results.remove(changed_results[0])
    rejects(verify_results, changed_results)


def test_configurations() -> None:
    for windows, profiles in ((False, ("release", "asan", "tsan")), (True, ("release", "asan"))):
        for profile in profiles:
            command = configure_command(Path("source"), Path("build"), profile, windows)
            for flag in (
                "-DBUILD_SHARED_LIBS=OFF", "-DGAMENET_BUILD_TESTING=ON",
                "-DGAMENET_BUILD_BENCHMARKS=ON", "-DGAMENET_BUILD_FUZZING=OFF",
                "-DGAMENET_ENABLE_EXPERIMENTAL=OFF", "-DGAMENET_ENABLE_TLS=OFF",
                f"-DGAMENET_ENABLE_ASAN_UBSAN={'ON' if profile == 'asan' else 'OFF'}",
                f"-DGAMENET_ENABLE_TSAN={'ON' if profile == 'tsan' else 'OFF'}",
            ):
                assert flag in command, (profile, flag)
            assert ("-A" in command) == windows
    rejects(lambda _: configure_command(Path("source"), Path("build"), "tsan", True), None)


def test_real_ctest_reports() -> None:
    # Validate the actual CTest JSON/JUnit formats without building Core.
    with tempfile.TemporaryDirectory(prefix="gamenet-prestudy-report-") as directory:
        root = Path(directory)
        cmake = shutil.which("cmake")
        assert cmake is not None, "CMake is required for the report-format contract"
        lines = []
        for name in CONTRACTS:
            lines.extend([
                f'add_test("{name}" "{Path(cmake).as_posix()}" -E true)',
                f'set_tests_properties("{name}" PROPERTIES LABELS "contract;experimental;prestudy")',
            ])
        (root / "CTestTestfile.cmake").write_text("\n".join(lines) + "\n", encoding="utf-8")
        inventory = subprocess.run(
            ["ctest", "--test-dir", str(root), "-L", "^prestudy$", "--show-only=json-v1"],
            check=True, capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
        verify_inventory(json.loads(inventory.stdout))
        junit = root / "results.xml"
        result = subprocess.run(
            ["ctest", "--test-dir", str(root), "-L", "^prestudy$", "--no-tests=error",
             "--parallel", "1", "--output-junit", str(junit)],
            check=False, capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        verify_results(ET.parse(junit).getroot())


def test_workflow() -> None:
    repo = Path(__file__).resolve().parents[2]
    workflow = (repo / ".github/workflows/prestudy-contracts.yml").read_text(encoding="utf-8")
    runner = (repo / "tests/ci/run_prestudy_contracts.py").read_text(encoding="utf-8")
    tests_cmake = (repo / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    root_cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    docs = (repo / "docs/development/ci.md").read_text(encoding="utf-8")

    for fragment in (
        "workflow_dispatch:", "schedule:", "cron: '23 3 * * 1'",
        "permissions:\n  contents: read", "timeout-minutes: 45",
        "fail-fast: false", "max-parallel: 2", "persist-credentials: false",
        "python tests/ci/test_prestudy_workflow.py", "python tests/cmake/test_sanitizer_flags.py",
        "python tests/ci/run_prestudy_contracts.py", "--profile ${{ matrix.profile }}",
        "--build-dir build-prestudy", "--output out/prestudy-evidence",
        "if: always()", "uses: actions/upload-artifact@v4", "if-no-files-found: error",
        "prestudy-${{ matrix.lane }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}",
        "path: out/prestudy-evidence/", "retention-days: 30",
    ):
        assert fragment in workflow, f"missing workflow boundary: {fragment}"
    assert not re.search(r"^  (push|pull_request|pull_request_target):", workflow, re.MULTILINE)
    assert "continue-on-error" not in workflow
    matrix = re.findall(r"- lane: (\S+)\n\s+os: (\S+)\n\s+profile: (\S+)", workflow)
    assert set(matrix) == {
        ("linux-release", "ubuntu-24.04", "release"),
        ("linux-asan-ubsan", "ubuntu-24.04", "asan"),
        ("linux-tsan", "ubuntu-24.04", "tsan"),
        ("windows-release", "windows-latest", "release"),
        ("windows-asan", "windows-latest", "asan"),
    }
    assert len(matrix) == 5
    for name, target in CONTRACTS.items():
        assert f"NAME {name}" in tests_cmake, name
        assert target in tests_cmake, target
    registered = re.findall(
        r"set_tests_properties\(\s*(\S+)\s+PROPERTIES LABELS \"[^\"]*;prestudy\"",
        tests_cmake,
    )
    assert len(registered) == len(CONTRACTS) and set(registered) == set(CONTRACTS)
    assert 'option(GAMENET_BUILD_BENCHMARKS "Build opt-in performance benchmarks" OFF)' in root_cmake
    for fragment in (
        '"--parallel", "2", "--target", *CONTRACTS.values()',
        '"-L", "^prestudy$", "--parallel", "1", "--timeout", "60"',
        '"--no-tests=error"', 'verify_inventory(json.loads(inventory))',
        'verify_results(ET.parse(junit).getroot())', '"promotion_eligible": False',
        '"evidence_class": "development-contracts"',
        'environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"',
        'environment["TSAN_OPTIONS"] = "halt_on_error=1"',
    ):
        assert fragment in runner, f"missing runner boundary: {fragment}"
    assert "--install" not in runner
    assert "run_hot_path_cost.py" not in runner
    assert "prestudy-contracts.yml" in docs
    assert "cannot satisfy HP0" in docs


def main() -> None:
    test_evidence_validation()
    test_configurations()
    test_real_ctest_reports()
    test_workflow()
    print("prestudy workflow and evidence rejection contracts passed")


if __name__ == "__main__":
    main()
