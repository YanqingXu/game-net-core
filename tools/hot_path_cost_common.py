#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import hashlib
import json
import math
import os
import platform as host_platform
import re
from pathlib import Path
from typing import Any

import validate_capacity_profile
import validate_core_benchmark
import validate_phase4_benchmark


LEDGER_SCHEMA = "gamenet.hot_path_cost.v1"
INVENTORY_SCHEMA = "gamenet.hot_path_cost_inventory.v1"
OBSERVATION_SCHEMA = "gamenet.hot_path_observation.v1"

COST_KEYS = (
    "cycles",
    "instructions",
    "llc_load_misses",
    "allocations",
    "copied_bytes",
    "syscalls",
    "wakeups",
    "generic_posts",
    "handoffs",
    "latency_p50_us",
    "latency_p99_us",
    "latency_p999_us",
    "queue_age_max_us",
    "rss_bytes",
    "overload_recovery_ms",
    "shutdown_convergence_us",
)

REQUIRED_DOMAINS = {
    "framing",
    "network_logic",
    "readiness",
    "send",
    "broadcast",
    "connection_capacity",
    "overload_recovery",
    "shutdown",
}


class HotPathCostError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise HotPathCostError(message)


def detect_host() -> dict[str, Any]:
    """Read the executing host; CLI labels are assertions, not evidence."""
    system = host_platform.system()
    release = host_platform.release()
    version = host_platform.version()
    platform_name = {"Linux": "linux", "Windows": "windows"}.get(system)
    kernel_identity = f"{release} {version}".lower()
    wsl = platform_name == "linux" and (
        "microsoft" in kernel_identity
        or "wsl" in kernel_identity
        or bool(os.environ.get("WSL_INTEROP") or os.environ.get("WSL_DISTRO_NAME"))
    )
    return {
        "platform": platform_name,
        "system": system,
        "release": release,
        "version": version,
        "machine": host_platform.machine(),
        "wsl": wsl,
    }


def validate_host_identity(
    platform: str, backend: str, native_runner: bool, wsl: bool,
) -> dict[str, Any]:
    host = detect_host()
    require(host["platform"] is not None, f"unsupported executing host: {host['system']}")
    require(platform == host["platform"], "declared platform does not match executing host")
    require(backend == ("epoll" if platform == "linux" else "iocp"), "platform/backend pair mismatch")
    require(wsl == host["wsl"], "declared WSL state does not match executing host")
    require(not (native_runner and host["wsl"]), "WSL cannot be declared a native runner")
    return host


def require_complete_costs(
    costs: dict[str, int | float | None], scenario: dict[str, Any], label: str,
) -> None:
    missing = [key for key in scenario["required_costs"] if costs.get(key) is None]
    require(not missing, f"{label}: missing required costs: {', '.join(missing)}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def finite_number(value: Any, label: str) -> int | float:
    valid = isinstance(value, (int, float)) and not isinstance(value, bool)
    try:
        valid = valid and math.isfinite(float(value))
    except (OverflowError, ValueError):
        valid = False
    require(valid, f"{label} must be a finite number")
    return value


def extract(document: Any, path: str, label: str) -> Any:
    current = document
    for component in path.split("."):
        require(isinstance(current, dict), f"{label}: {path} crosses a non-object")
        require(component in current, f"{label}: missing {path}")
        current = current[component]
    return current


def extract_scaled(document: dict[str, Any], source: Any, label: str) -> Any:
    if isinstance(source, str):
        return extract(document, source, label)
    require(isinstance(source, dict), f"{label}: source must be a string or object")
    require(set(source).issubset({"source", "scale"}), f"{label}: unsupported source key")
    path = source.get("source")
    scale = source.get("scale", 1.0)
    require(isinstance(path, str) and path, f"{label}: source path missing")
    finite_number(scale, f"{label}: scale")
    value = extract(document, path, label)
    if value is None:
        return None
    return finite_number(value, label) * scale


def load_inventory(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise HotPathCostError(f"cannot read inventory {path}: {error}") from error
    require(isinstance(document, dict), "hot-path inventory must be an object")
    require(document.get("schema") == INVENTORY_SCHEMA, "hot-path inventory schema mismatch")
    require(tuple(document.get("cost_keys", ())) == COST_KEYS, "hot-path cost-key order drifted")
    scenarios = document.get("scenarios")
    require(isinstance(scenarios, list) and scenarios, "hot-path inventory has no scenarios")
    keys: set[str] = set()
    domains: set[str] = set()
    for index, scenario in enumerate(scenarios):
        label = f"inventory scenario {index}"
        require(isinstance(scenario, dict), f"{label} must be an object")
        key = scenario.get("key")
        require(
            isinstance(key, str)
            and re.fullmatch(r"[a-z0-9][a-z0-9.-]*", key) is not None
            and key not in keys,
            f"{label} has an invalid key",
        )
        keys.add(key)
        scenario_domains = scenario.get("domains")
        require(isinstance(scenario_domains, list) and scenario_domains, f"{key}: domains missing")
        require(all(isinstance(item, str) and item for item in scenario_domains), f"{key}: invalid domain")
        domains.update(scenario_domains)
        require(scenario.get("executable") in {"core", "phase4", "capacity", "queued"}, f"{key}: invalid executable")
        require(isinstance(scenario.get("child_schema"), str), f"{key}: child schema missing")
        arguments = scenario.get("arguments")
        require(isinstance(arguments, list) and all(isinstance(item, str) for item in arguments), f"{key}: arguments must be strings")
        require(isinstance(scenario.get("baseline"), str) and scenario["baseline"], f"{key}: baseline missing")
        hp_slices = scenario.get("hp_slices")
        require(isinstance(hp_slices, list) and hp_slices, f"{key}: HP slice list missing")
        for metric_group in ("primary",):
            metric = scenario.get(metric_group)
            require(isinstance(metric, dict), f"{key}: {metric_group} metric missing")
            require(metric.get("direction") in {"higher", "lower", "exact"}, f"{key}: invalid metric direction")
            require(isinstance(metric.get("name"), str) and isinstance(metric.get("source"), str), f"{key}: malformed metric")
        guardrails = scenario.get("guardrails")
        require(isinstance(guardrails, list) and guardrails, f"{key}: guardrails missing")
        for guardrail in guardrails:
            require(isinstance(guardrail, dict), f"{key}: guardrail must be an object")
            require(guardrail.get("direction") in {"higher", "lower", "exact"}, f"{key}: invalid guardrail direction")
            require(isinstance(guardrail.get("name"), str) and isinstance(guardrail.get("source"), str), f"{key}: malformed guardrail")
        sources = scenario.get("cost_sources")
        require(isinstance(sources, dict) and set(sources).issubset(COST_KEYS), f"{key}: invalid cost sources")
        required_costs = scenario.get("required_costs")
        require(isinstance(required_costs, list) and required_costs, f"{key}: required costs missing")
        require(len(set(required_costs)) == len(required_costs), f"{key}: duplicate required cost")
        require(set(required_costs).issubset(COST_KEYS), f"{key}: unknown required cost")
    require(REQUIRED_DOMAINS.issubset(domains), "hot-path inventory does not cover every HP0 domain")
    return document


def _validate_queued(document: Any, build_type: str, label: str) -> dict[str, Any]:
    require(isinstance(document, dict), f"{label}: output must be an object")
    require(document.get("schema") == "gamenet.multi_io_queued_benchmark.v1", f"{label}: schema mismatch")
    require(document.get("status") == "ok", f"{label}: status is not ok")
    require(document.get("build_type") == build_type, f"{label}: build type mismatch")
    for key in (
        "clients",
        "io_threads",
        "messages_per_client",
        "payload_bytes",
        "messages_per_second",
        "mebibytes_per_second",
        "network_to_logic_p99_us",
        "network_to_logic_p999_us",
        "logic_to_network_p99_us",
        "logic_to_network_p999_us",
        "queue_oldest_age_max_us",
        "queue_depth_high_watermark",
        "working_set_before_bytes",
        "working_set_after_bytes",
        "working_set_delta_bytes",
        "producer_wake_posts",
        "producer_wake_merges",
        "drain_callbacks",
        "cross_domain_handoffs",
        "shutdown_us",
    ):
        finite_number(document.get(key), f"{label}: {key}")
    messages = document["clients"] * document["messages_per_client"]
    require(document["io_threads"] == 2, f"{label}: queued profile must use two I/O owners")
    require(document["producer_wake_posts"] + document["producer_wake_merges"] == messages, f"{label}: wake accounting mismatch")
    require(document["cross_domain_handoffs"] == 2 * messages, f"{label}: handoff accounting mismatch")
    require(document["network_to_logic_p999_us"] >= document["network_to_logic_p99_us"], f"{label}: network-to-logic percentile order invalid")
    require(document["logic_to_network_p999_us"] >= document["logic_to_network_p99_us"], f"{label}: logic-to-network percentile order invalid")
    return document


def validate_child(
    document: Any,
    scenario: dict[str, Any],
    platform: str,
    backend: str,
    build_type: str,
    label: str,
) -> dict[str, Any]:
    group = scenario["executable"]
    validate_preregistered_arguments(document, scenario, label)
    try:
        if group == "core":
            expected = "echo" if scenario["key"].startswith("readiness-send") else "connections"
            return validate_core_benchmark.validate_document(
                document,
                expected_platform=platform,
                expected_backend=backend,
                expected_build_type=build_type,
                expected_scenario=expected,
                label=label,
            )
        if group == "phase4":
            expected = "framing" if scenario["key"].startswith("framing") else "broadcast-fanout"
            return validate_phase4_benchmark.validate_document(
                document,
                expected,
                platform,
                backend,
                build_type,
                Path(label),
            )
        if group == "capacity":
            return validate_capacity_profile.validate_document(
                document,
                expected_platform=platform,
                expected_backend=backend,
                expected_build_type=build_type,
                expected_connections=100,
                label=label,
            )
        return _validate_queued(document, build_type, label)
    except HotPathCostError:
        raise
    except ValueError as error:
        raise HotPathCostError(str(error)) from error


def validate_preregistered_arguments(
    document: Any, scenario: dict[str, Any], label: str,
) -> None:
    """A self-consistent shortened workload is not the preregistered scenario."""
    group = scenario["executable"]
    arguments = scenario["arguments"]
    if group == "queued":
        require(len(arguments) == 3, f"{label}: queued inventory arguments drifted")
        fields = ("messages_per_client", "payload_bytes", "clients")
        pairs = zip(fields, arguments, strict=True)
    else:
        common = {"--scenario": "scenario", "--timeout-ms": "parameters.timeout_ms"}
        mapping = {
            "core": {
                "--connections": "parameters.connections",
                "--threads": "parameters.event_loop_threads",
                "--messages": "parameters.messages_per_connection",
                "--payload": "parameters.payload_bytes",
                "--settle-ms": "parameters.settle_ms",
            },
            "phase4": {
                "--messages": "parameters.messages", "--payload": "parameters.payload_bytes",
                "--threads": "parameters.threads", "--batch": "parameters.batch_size",
                "--fanout": "parameters.fanout", "--tick-us": "parameters.tick_interval_us",
            },
            "capacity": {
                "--connections": "parameters.connections", "--threads": "parameters.threads",
                "--messages": "parameters.messages", "--payload-bytes": "parameters.payload_bytes",
                "--server-send-buffer-bytes": "parameters.server_send_buffer_bytes",
                "--pressure-settle-ms": "parameters.pressure_settle_ms",
                "--recovery-stable-ms": "parameters.recovery_stable_ms",
                "--iocp-accept-depth": "parameters.iocp_accept_depth",
                "--low-water-bytes": "limits.connection_low_water_bytes",
                "--high-water-bytes": "limits.connection_high_water_bytes",
                "--hard-limit-bytes": "limits.connection_hard_limit_bytes",
                "--recovery-threshold-bytes": "limits.recovery_pending_threshold_bytes",
            },
        }
        require(group in mapping, f"{label}: unknown executable group")
        common.update(mapping[group])
        require(len(arguments) % 2 == 0, f"{label}: inventory option/value pairing drifted")
        options = list(zip(arguments[::2], arguments[1::2], strict=True))
        require(len({option for option, _ in options}) == len(options), f"{label}: duplicate inventory option")
        require(all(option in common for option, _ in options), f"{label}: unsupported inventory option")
        pairs = ((common[option], value) for option, value in options)
    for path, expected in pairs:
        actual = extract(document, path, label)
        if path != "scenario":
            try:
                expected = int(expected)
            except ValueError as error:
                raise HotPathCostError(f"{label}: invalid inventory integer for {path}") from error
            require(isinstance(actual, int) and not isinstance(actual, bool), f"{label}: {path} must be an integer")
        require(actual == expected, f"{label}: {path} differs from preregistered arguments")


def child_parameters(document: dict[str, Any], scenario: dict[str, Any]) -> dict[str, Any]:
    if isinstance(document.get("parameters"), dict):
        return document["parameters"]
    return {
        "arguments": scenario["arguments"],
        "clients": document["clients"],
        "io_threads": document["io_threads"],
        "messages_per_client": document["messages_per_client"],
        "payload_bytes": document["payload_bytes"],
    }


def metric_record(document: dict[str, Any], specification: dict[str, Any], label: str) -> dict[str, Any]:
    value = extract(document, specification["source"], label)
    if specification["direction"] == "exact":
        require(isinstance(value, (bool, int, float, str)), f"{label}: exact metric is not scalar")
    else:
        finite_number(value, label)
    return {
        "name": specification["name"],
        "direction": specification["direction"],
        "value": value,
    }


def load_observation(path: Path) -> dict[str, int | float | None]:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            require(key not in result, f"observer output has duplicate JSON key: {key}")
            result[key] = value
        return result

    try:
        document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    except (OSError, ValueError) as error:
        raise HotPathCostError(f"cannot read observer output {path}: {error}") from error
    require(isinstance(document, dict), "observer output must be an object")
    require(document.get("schema") == OBSERVATION_SCHEMA, "observer output schema mismatch")
    costs = document.get("costs")
    require(isinstance(costs, dict) and set(costs) == set(COST_KEYS), "observer output cost keys drifted")
    normalized: dict[str, int | float | None] = {}
    for key in COST_KEYS:
        value = costs[key]
        if value is not None:
            finite_number(value, f"observer cost {key}")
            require(value >= 0, f"observer cost {key} must be non-negative")
        normalized[key] = value
    return normalized
