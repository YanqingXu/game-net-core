from __future__ import annotations

import json
import re
import subprocess
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path


PHASE4_ARTIFACTS = {
    "intents/modules/packet_framer.intent.md": ("installed-library", "GameNet::protocol"),
    "intents/modules/transport_endpoint.intent.md": ("installed-library", "GameNet::transport"),
    "intents/modules/player_session.intent.md": ("installed-library", "GameNet::game_session"),
    "intents/modules/logic_loop.intent.md": ("installed-library", "GameNet::game_logic"),
    "intents/modules/broadcast.intent.md": ("installed-library", "GameNet::broadcast"),
    "intents/usecases/game_server_pipeline_demo.intent.md": (
        "example",
        "gamenet_game_server_pipeline_demo",
    ),
    "intents/usecases/phase4_performance_baseline.intent.md": (
        "benchmark",
        "gamenet_phase4_benchmark",
    ),
}

CONCRETE_ACTIVE_ARTIFACTS = {
    "intents/usecases/echo_server.intent.md": ("example", "gamenet_echo_server"),
    "intents/usecases/core_performance_baseline.intent.md": (
        "benchmark",
        "gamenet_core_benchmark",
    ),
    **PHASE4_ARTIFACTS,
}

FROZEN_CORE_ACTIVE_INTENTS = {
    "intents/architecture/lifetime_rules.intent.md",
    "intents/architecture/threading_model.intent.md",
    "intents/modules/acceptor.intent.md",
    "intents/modules/buffer.intent.md",
    "intents/modules/channel.intent.md",
    "intents/modules/connector.intent.md",
    "intents/modules/event_loop.intent.md",
    "intents/modules/event_loop_thread.intent.md",
    "intents/modules/event_loop_thread_pool.intent.md",
    "intents/modules/logger.intent.md",
    "intents/modules/platform_runtime.intent.md",
    "intents/modules/poller.intent.md",
    "intents/modules/tcp_client.intent.md",
    "intents/modules/tcp_connection.intent.md",
    "intents/modules/tcp_server.intent.md",
    "intents/modules/timer_queue.intent.md",
}

PRODUCTION_TARGET_DEPENDENCIES = {
    "gamenet_core": set(),
    "gamenet_protocol": {"gamenet_core"},
    "gamenet_transport": {"gamenet_core"},
    "gamenet_game_session": {"gamenet_core", "gamenet_transport"},
    "gamenet_game_logic": {"gamenet_core", "gamenet_transport", "gamenet_game_session"},
    "gamenet_broadcast": {"gamenet_core", "gamenet_transport", "gamenet_game_session"},
}

ARTIFACT_KINDS = {"installed-library", "example", "benchmark"}
MIGRATION_MODES = {"adapt", "redesign", "native"}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
INSTALL_KEYWORDS = {
    "ARCHIVE",
    "BUNDLE",
    "COMPONENT",
    "CONFIGURATIONS",
    "DESTINATION",
    "EXPORT",
    "FRAMEWORK",
    "INCLUDES",
    "LIBRARY",
    "NAMELINK_COMPONENT",
    "OBJECTS",
    "OPTIONAL",
    "PERMISSIONS",
    "PRIVATE_HEADER",
    "PUBLIC_HEADER",
    "RESOURCE",
    "RUNTIME",
}
LINK_KEYWORDS = {
    "BEFORE",
    "DEBUG",
    "GENERAL",
    "INTERFACE",
    "LINK_INTERFACE_LIBRARIES",
    "LINK_PRIVATE",
    "LINK_PUBLIC",
    "OPTIMIZED",
    "PRIVATE",
    "PUBLIC",
}


@dataclass
class ConfiguredTarget:
    kind: str
    sources: set[str] = field(default_factory=set)


@dataclass
class ConfiguredInventory:
    targets: dict[str, ConfiguredTarget]
    aliases: dict[str, str]
    installed_targets: set[str]
    test_targets: set[str]
    tests_by_source: dict[str, set[str]]
    ctest_names: set[str]
    link_dependencies: dict[str, set[str]]


def require(condition: bool, message: str) -> None:
    assert condition, message


def run(command: list[str], *, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
    )
    require(
        result.returncode == 0,
        "command failed:\n"
        + " ".join(command)
        + f"\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
    )
    return result


def parse_front_matter(path: Path) -> tuple[dict[str, str], str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    require(lines and lines[0] == "---", f"missing metadata start: {path}")
    try:
        end = lines.index("---", 1)
    except ValueError as error:
        raise AssertionError(f"missing metadata end: {path}") from error

    metadata: dict[str, str] = {}
    for line in lines[1:end]:
        match = re.fullmatch(r"([a-z_]+): (\S+)", line)
        require(match is not None, f"invalid metadata scalar in {path}: {line!r}")
        key, value = match.groups()
        require(key not in metadata, f"duplicate metadata key in {path}: {key}")
        metadata[key] = value
    return metadata, "\n".join(lines[end + 1 :])


def active_intents_from_catalog(repo_root: Path) -> set[str]:
    active: set[str] = set()
    in_active_catalog = False
    for line in (repo_root / "intents" / "README.md").read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped == "## Active Intents":
            in_active_catalog = True
            continue
        if stripped.startswith("## "):
            in_active_catalog = False
            continue
        if not in_active_catalog:
            continue
        match = re.fullmatch(r"- `([^`]+\.intent\.md)`", stripped)
        if match is not None:
            active.add(match.group(1))
    require(active, "active intent catalog is empty")
    return active


def active_intents_from_front_matter(repo_root: Path) -> set[str]:
    active: set[str] = set()
    for path in (repo_root / "intents").rglob("*.intent.md"):
        metadata, _ = parse_front_matter(path)
        if metadata.get("status") == "active":
            active.add(path.relative_to(repo_root).as_posix())
    require(active, "no active intent front matter was found")
    return active


def active_intent_paths(repo_root: Path) -> list[str]:
    catalog = active_intents_from_catalog(repo_root)
    front_matter = active_intents_from_front_matter(repo_root)
    require(
        catalog == front_matter,
        "active intent catalog/front matter mismatch: "
        f"catalog-only={sorted(catalog - front_matter)}, "
        f"front-matter-only={sorted(front_matter - catalog)}",
    )
    return sorted(catalog)


def project_cmake_files(source_root: Path) -> list[Path]:
    paths = [source_root / "CMakeLists.txt"]
    for directory in ("src", "examples", "benchmarks", "tests"):
        root = source_root / directory
        if root.is_dir():
            paths.extend(root.rglob("CMakeLists.txt"))
    return sorted({path.resolve() for path in paths if path.is_file()})


def normalized_source(argument: str, command_file: Path, source_root: Path) -> str | None:
    if "$<" in argument or Path(argument).suffix.lower() not in SOURCE_SUFFIXES:
        return None
    candidate = Path(argument)
    if not candidate.is_absolute():
        candidate = command_file.parent / candidate
    candidate = candidate.resolve()
    try:
        return candidate.relative_to(source_root.resolve()).as_posix()
    except ValueError:
        return candidate.as_posix()


def trace_commands(output: str) -> list[dict[str, object]]:
    commands: list[dict[str, object]] = []
    for line in output.splitlines():
        if not line.startswith("{"):
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(record, dict) and "cmd" in record and "args" in record:
            commands.append(record)
    return commands


def add_test_identity(arguments: list[str]) -> tuple[str, str] | None:
    if not arguments:
        return None
    if arguments[0] == "NAME":
        if len(arguments) < 4 or "COMMAND" not in arguments:
            return None
        command_index = arguments.index("COMMAND")
        if command_index + 1 >= len(arguments):
            return None
        return arguments[1], arguments[command_index + 1]
    if len(arguments) >= 2:
        return arguments[0], arguments[1]
    return None


def configure_inventory(
    source_root: Path,
    build_root: Path,
    definitions: dict[str, str] | None = None,
) -> ConfiguredInventory:
    command = [
        "cmake",
        "-S",
        str(source_root),
        "-B",
        str(build_root),
        "--trace-expand",
        "--trace-format=json-v1",
        "-Wno-dev",
    ]
    for path in project_cmake_files(source_root):
        command.append(f"--trace-source={path}")
    for key, value in sorted((definitions or {}).items()):
        command.append(f"-D{key}={value}")

    configured = run(command)
    commands = trace_commands(configured.stdout + "\n" + configured.stderr)
    require(commands, f"CMake configure produced no executable trace inventory: {source_root}")

    targets: dict[str, ConfiguredTarget] = {}
    aliases: dict[str, str] = {}
    installed_targets: set[str] = set()
    configured_tests: dict[str, str] = {}
    raw_link_dependencies: dict[str, set[str]] = {}

    for record in commands:
        cmake_command = str(record["cmd"]).lower()
        arguments = [str(value) for value in record["args"]]
        command_file = Path(str(record["file"]))

        if cmake_command == "add_library" and arguments:
            if len(arguments) >= 3 and arguments[1] == "ALIAS":
                aliases[arguments[0]] = arguments[2]
                continue
            target = arguments[0]
            entry = targets.setdefault(target, ConfiguredTarget(kind="library"))
            for argument in arguments[1:]:
                source = normalized_source(argument, command_file, source_root)
                if source is not None:
                    entry.sources.add(source)
        elif cmake_command == "add_executable" and arguments:
            target = arguments[0]
            entry = targets.setdefault(target, ConfiguredTarget(kind="executable"))
            for argument in arguments[1:]:
                source = normalized_source(argument, command_file, source_root)
                if source is not None:
                    entry.sources.add(source)
        elif cmake_command == "install" and arguments and arguments[0] == "TARGETS":
            for argument in arguments[1:]:
                if argument.upper() in INSTALL_KEYWORDS:
                    break
                installed_targets.add(argument)
        elif cmake_command == "target_link_libraries" and len(arguments) >= 2:
            dependencies = raw_link_dependencies.setdefault(arguments[0], set())
            for argument in arguments[1:]:
                for item in argument.split(";"):
                    if item and item.upper() not in LINK_KEYWORDS and not item.startswith("-"):
                        dependencies.add(item)
        elif cmake_command == "add_test":
            identity = add_test_identity(arguments)
            if identity is not None:
                name, target = identity
                require(name not in configured_tests, f"duplicate configured CTest name: {name}")
                configured_tests[name] = target

    ctest_result = run(
        [
            "ctest",
            "--test-dir",
            str(build_root),
            "-C",
            "Debug",
            "--show-only=json-v1",
        ]
    )
    try:
        ctest_inventory = json.loads(ctest_result.stdout)
    except json.JSONDecodeError as error:
        raise AssertionError(f"CTest inventory is not JSON: {ctest_result.stdout}") from error
    ctest_names = {str(test["name"]) for test in ctest_inventory.get("tests", [])}
    trace_names = set(configured_tests)
    require(
        trace_names == ctest_names,
        "CMake add_test trace and configured CTest inventory disagree: "
        f"trace-only={sorted(trace_names - ctest_names)}, "
        f"ctest-only={sorted(ctest_names - trace_names)}",
    )

    test_targets = set(configured_tests.values())
    tests_by_source: dict[str, set[str]] = {}
    for test_name, target in configured_tests.items():
        target_name = aliases.get(target, target)
        target_record = targets.get(target_name)
        if target_record is None:
            continue
        for source in target_record.sources:
            tests_by_source.setdefault(source, set()).add(test_name)

    link_dependencies: dict[str, set[str]] = {target: set() for target in targets}
    for raw_target, raw_dependencies in raw_link_dependencies.items():
        target = aliases.get(raw_target, raw_target)
        if target not in targets:
            continue
        for raw_dependency in raw_dependencies:
            dependency = aliases.get(raw_dependency, raw_dependency)
            if dependency in targets:
                link_dependencies[target].add(dependency)

    return ConfiguredInventory(
        targets=targets,
        aliases=aliases,
        installed_targets=installed_targets,
        test_targets=test_targets,
        tests_by_source=tests_by_source,
        ctest_names=ctest_names,
        link_dependencies=link_dependencies,
    )


def workflow_python_scripts(repo_root: Path) -> set[str]:
    scripts: set[str] = set()
    workflow_root = repo_root / ".github" / "workflows"
    if not workflow_root.is_dir():
        return scripts
    for workflow in [*workflow_root.glob("*.yml"), *workflow_root.glob("*.yaml")]:
        text = workflow.read_text(encoding="utf-8")
        for match in re.finditer(
            r"^\s*(?:python(?:3)?|py\s+-3)\s+([^\s#]+\.py)\b",
            text,
            flags=re.MULTILINE,
        ):
            scripts.add(match.group(1).replace("\\", "/").removeprefix("./"))
    return scripts


def verification_paths(body: str) -> set[str]:
    return {
        value
        for value in re.findall(
            r"(?<![A-Za-z0-9_./-])(tests/[A-Za-z0-9_./-]+\.(?:cpp|py))(?![A-Za-z0-9_./-])",
            body,
        )
    }


def transitive_dependencies(inventory: ConfiguredInventory, target: str) -> set[str]:
    reached: set[str] = set()
    pending = list(inventory.link_dependencies.get(target, set()))
    while pending:
        dependency = pending.pop()
        if dependency in reached:
            continue
        reached.add(dependency)
        pending.extend(inventory.link_dependencies.get(dependency, set()) - reached)
    return reached


def validate_dependency_direction(inventory: ConfiguredInventory) -> None:
    production_targets = set(PRODUCTION_TARGET_DEPENDENCIES)
    for target, allowed in PRODUCTION_TARGET_DEPENDENCIES.items():
        if target not in inventory.targets:
            continue
        reached = transitive_dependencies(inventory, target)
        require(target not in reached, f"configured target dependency cycle reaches {target}")
        forbidden = sorted((reached & production_targets) - allowed)
        require(
            not forbidden,
            f"configured dependency direction violation: {target} reaches {forbidden}",
        )


def validate_artifact(
    metadata: dict[str, str],
    inventory: ConfiguredInventory,
    intent_name: str,
) -> None:
    kind = metadata.get("artifact_kind", "")
    target = metadata.get("target", "")
    require(kind in ARTIFACT_KINDS, f"invalid artifact kind in {intent_name}: {kind}")
    require(target, f"missing artifact target in {intent_name}")

    if kind == "installed-library":
        require(
            target in inventory.aliases,
            f"active installed target does not resolve to a configured alias: {intent_name}: {target}",
        )
    elif "::" in target:
        require(
            target in inventory.aliases,
            f"active target does not resolve to a configured alias: {intent_name}: {target}",
        )

    backing_target = inventory.aliases.get(target, target)
    configured_target = inventory.targets.get(backing_target)
    require(
        configured_target is not None,
        f"active target does not exist in configured CMake inventory: {intent_name}: {target}",
    )

    if kind == "installed-library":
        require(
            configured_target.kind == "library",
            f"installed-library intent resolves to a non-library target: {intent_name}: {target}",
        )
        require(
            backing_target in inventory.installed_targets,
            f"active installed-library target is not installed: {intent_name}: {target}",
        )
    else:
        require(
            configured_target.kind == "executable",
            f"{kind} intent resolves to a non-executable target: {intent_name}: {target}",
        )
        require(
            backing_target not in inventory.installed_targets,
            f"{kind} intent unexpectedly names an installed target: {intent_name}: {target}",
        )
        require(
            backing_target not in inventory.test_targets,
            f"{kind} intent unexpectedly names a CTest executable: {intent_name}: {target}",
        )


def validate_frozen_core_artifact(
    metadata: dict[str, str],
    inventory: ConfiguredInventory,
    intent_name: str,
) -> None:
    require(
        "artifact_kind" not in metadata,
        f"frozen Core intent unexpectedly carries partial artifact metadata: {intent_name}",
    )
    require(
        metadata.get("target") == "GameNet::core",
        f"frozen Core intent must target GameNet::core: {intent_name}",
    )
    validate_artifact(
        {**metadata, "artifact_kind": "installed-library"},
        inventory,
        intent_name,
    )


def validate_provenance(metadata: dict[str, str], repo_root: Path, intent_name: str) -> None:
    require(metadata.get("migration_source") == "mini_trantor", f"missing source project: {intent_name}")
    require(
        metadata.get("migration_mode") in MIGRATION_MODES,
        f"missing migration mode: {intent_name}",
    )
    require(
        re.fullmatch(r"[0-9a-f]{40}", metadata.get("source_commit", "")) is not None,
        f"missing immutable source commit: {intent_name}",
    )
    source_paths = metadata.get("source_paths", "").split(";")
    require(source_paths and source_paths != [""], f"missing source paths: {intent_name}")
    for source_path in source_paths:
        require(
            (repo_root / "mini_trantor" / source_path).is_file(),
            f"migration source path does not exist: {intent_name}: {source_path}",
        )


def validate_verification(
    body: str,
    repo_root: Path,
    inventory: ConfiguredInventory,
    ci_scripts: set[str],
    fuzz_cmake: str,
    intent_name: str,
    require_path: bool,
) -> None:
    paths = verification_paths(body)
    if require_path:
        require(paths, f"active intent must name at least one verification path: {intent_name}")
    for test_path in paths:
        require(
            (repo_root / test_path).is_file(),
            f"intent verification file does not exist: {test_path}",
        )
        if test_path.startswith("tests/fuzz/"):
            # The optional libFuzzer build has a dedicated Clang-only contract.
            # Keep its source linkage here while configured CTest proves every
            # ordinary C++ verification path below.
            fuzz_relative = test_path.removeprefix("tests/fuzz/")
            require(
                fuzz_relative in fuzz_cmake,
                f"intent fuzz file is not registered with a fuzz target: {test_path}",
            )
        elif test_path.endswith(".cpp"):
            require(
                test_path in inventory.tests_by_source,
                f"intent verification file is not in configured CTest inventory: {test_path}",
            )
        else:
            require(
                test_path in ci_scripts,
                f"intent Python verification is not executed by a workflow: {test_path}",
            )


def validate_intent(
    path: Path,
    repo_root: Path,
    inventory: ConfiguredInventory,
    ci_scripts: set[str],
    fuzz_cmake: str,
    expected: tuple[str, str] | None = None,
    *,
    frozen_core: bool = False,
    require_provenance: bool = True,
    require_verification_path: bool = True,
) -> None:
    metadata, body = parse_front_matter(path)
    try:
        intent_name = path.relative_to(repo_root).as_posix()
    except ValueError:
        intent_name = path.as_posix()

    require(metadata.get("status") == "active", f"implemented artifact must be active: {intent_name}")
    if expected is not None:
        expected_kind, expected_target = expected
        require(
            metadata.get("artifact_kind") == expected_kind,
            f"artifact kind mismatch in {intent_name}: expected {expected_kind}",
        )
        require(
            metadata.get("target") == expected_target,
            f"artifact target mismatch in {intent_name}: expected {expected_target}",
        )

    if frozen_core:
        validate_frozen_core_artifact(metadata, inventory, intent_name)
    else:
        validate_artifact(metadata, inventory, intent_name)
    if require_provenance:
        validate_provenance(metadata, repo_root, intent_name)
    validate_verification(
        body,
        repo_root,
        inventory,
        ci_scripts,
        fuzz_cmake,
        intent_name,
        require_verification_path,
    )


def write_fixture_intent(
    path: Path,
    metadata: dict[str, str],
    verification_path: str,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    metadata_lines = "\n".join(f"{key}: {value}" for key, value in metadata.items())
    path.write_text(
        f"---\n{metadata_lines}\n---\n\n"
        f"# Isolated Intent Semantic Fixture\n\n"
        f"## Verification\n\n- `{verification_path}`\n",
        encoding="utf-8",
    )


def expect_failure(name: str, expected_message: str, callback: Callable[[], None]) -> None:
    try:
        callback()
    except AssertionError as error:
        require(
            expected_message in str(error),
            f"negative fixture {name} failed for the wrong reason: {error}",
        )
    else:
        raise AssertionError(f"negative fixture unexpectedly passed: {name}")


def run_negative_fixtures(temp_root: Path) -> None:
    source_root = temp_root / "semantic-negative-source"
    build_root = temp_root / "semantic-negative-build"
    (source_root / "tests").mkdir(parents=True)
    (source_root / "mini_trantor" / "legacy").mkdir(parents=True)

    for relative_path in (
        "fixture.cpp",
        "tests/registered.cpp",
        "tests/unregistered.cpp",
        "tests/false_registered.cpp",
        "mini_trantor/legacy/fixture.cpp",
    ):
        (source_root / relative_path).write_text("int fixture_symbol = 0;\n", encoding="utf-8")

    (source_root / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.20)
project(IntentSemanticNegativeFixture LANGUAGES CXX)
enable_testing()

add_library(real_lib STATIC fixture.cpp)
add_library(Fixture::real ALIAS real_lib)
install(TARGETS real_lib EXPORT FixtureTargets DESTINATION lib)
install(EXPORT FixtureTargets DESTINATION lib/cmake/Fixture)

add_executable(real_example fixture.cpp)
add_executable(real_benchmark fixture.cpp)
add_executable(real_test tests/registered.cpp)
add_test(NAME contract.fixture.registered COMMAND real_test)

# add_executable(comment_target fixture.cpp)
# add_test(NAME contract.fixture.comment COMMAND comment_target)

if(FALSE)
    add_executable(false_target fixture.cpp)
    add_executable(false_test tests/false_registered.cpp)
    add_test(NAME contract.fixture.false COMMAND false_test)
endif()
""",
        encoding="utf-8",
    )

    inventory = configure_inventory(source_root, build_root)
    base_metadata = {
        "status": "active",
        "target": "real_example",
        "migration_source": "mini_trantor",
        "promote_gate": "none",
        "artifact_kind": "example",
        "migration_mode": "redesign",
        "source_commit": "a" * 40,
        "source_paths": "legacy/fixture.cpp",
    }

    def fixture(name: str, metadata: dict[str, str], verification: str) -> Path:
        path = source_root / "intents" / f"{name}.intent.md"
        write_fixture_intent(path, metadata, verification)
        return path

    positive = fixture("positive", dict(base_metadata), "tests/registered.cpp")
    validate_intent(positive, source_root, inventory, set(), "")

    cases: list[tuple[str, str, Path]] = []

    missing_target = dict(base_metadata, target="missing_target")
    cases.append(
        (
            "missing-target",
            "active target does not exist in configured CMake inventory",
            fixture("missing_target", missing_target, "tests/registered.cpp"),
        )
    )

    wrong_kind = dict(base_metadata, target="Fixture::real", artifact_kind="example")
    cases.append(
        (
            "wrong-artifact-kind",
            "example intent resolves to a non-executable target",
            fixture("wrong_kind", wrong_kind, "tests/registered.cpp"),
        )
    )

    cases.append(
        (
            "missing-ctest-registration",
            "intent verification file is not in configured CTest inventory",
            fixture("missing_test", dict(base_metadata), "tests/unregistered.cpp"),
        )
    )

    cases.append(
        (
            "missing-test-path",
            "intent verification file does not exist",
            fixture("missing_test_path", dict(base_metadata), "tests/does_not_exist.cpp"),
        )
    )

    comment_target = dict(base_metadata, target="comment_target")
    cases.append(
        (
            "comment-only-target",
            "active target does not exist in configured CMake inventory",
            fixture("comment_target", comment_target, "tests/registered.cpp"),
        )
    )

    cases.append(
        (
            "if-false-test-registration",
            "intent verification file is not in configured CTest inventory",
            fixture("false_test", dict(base_metadata), "tests/false_registered.cpp"),
        )
    )

    missing_provenance = dict(base_metadata)
    del missing_provenance["source_commit"]
    cases.append(
        (
            "missing-provenance",
            "missing immutable source commit",
            fixture("missing_provenance", missing_provenance, "tests/registered.cpp"),
        )
    )

    for name, expected_message, path in cases:
        expect_failure(
            name,
            expected_message,
            lambda path=path: validate_intent(path, source_root, inventory, set(), ""),
        )


def dependency_fixture_inventory(
    temp_root: Path,
    name: str,
    dependency_commands: str,
) -> ConfiguredInventory:
    source_root = temp_root / f"dependency-{name}-source"
    build_root = temp_root / f"dependency-{name}-build"
    source_root.mkdir(parents=True)
    (source_root / "fixture.cpp").write_text("int dependency_fixture = 0;\n", encoding="utf-8")
    (source_root / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.20)
project(IntentDependencyFixture LANGUAGES CXX)
enable_testing()

add_library(gamenet_core STATIC fixture.cpp)
add_library(GameNet::core ALIAS gamenet_core)
add_library(gamenet_protocol STATIC fixture.cpp)
add_library(GameNet::protocol ALIAS gamenet_protocol)
add_library(reverse_bridge INTERFACE)
"""
        + dependency_commands,
        encoding="utf-8",
    )
    return configure_inventory(source_root, build_root)


def run_dependency_direction_fixtures(temp_root: Path) -> None:
    allowed = dependency_fixture_inventory(
        temp_root,
        "allowed",
        "target_link_libraries(gamenet_protocol PUBLIC GameNet::core)\n",
    )
    validate_dependency_direction(allowed)

    direct_reverse = dependency_fixture_inventory(
        temp_root,
        "direct-reverse",
        "target_link_libraries(gamenet_core PUBLIC GameNet::protocol)\n",
    )
    expect_failure(
        "top-level-direct-reverse-dependency",
        "configured dependency direction violation: gamenet_core reaches ['gamenet_protocol']",
        lambda: validate_dependency_direction(direct_reverse),
    )

    transitive_reverse = dependency_fixture_inventory(
        temp_root,
        "transitive-reverse",
        "target_link_libraries(reverse_bridge INTERFACE GameNet::protocol)\n"
        "target_link_libraries(gamenet_core PUBLIC reverse_bridge)\n",
    )
    expect_failure(
        "top-level-transitive-reverse-dependency",
        "configured dependency direction violation: gamenet_core reaches ['gamenet_protocol']",
        lambda: validate_dependency_direction(transitive_reverse),
    )


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    fuzz_cmake = (repo_root / "tests" / "fuzz" / "CMakeLists.txt").read_text(encoding="utf-8")
    ci_scripts = workflow_python_scripts(repo_root)
    active_paths = active_intent_paths(repo_root)
    active_set = set(active_paths)
    explicit_verification_path_count = sum(
        len(verification_paths(parse_front_matter(repo_root / relative_path)[1]))
        for relative_path in active_paths
    )
    require(
        FROZEN_CORE_ACTIVE_INTENTS <= active_set,
        "frozen Core semantic exemptions reference non-active intents: "
        f"{sorted(FROZEN_CORE_ACTIVE_INTENTS - active_set)}",
    )
    require(
        set(PHASE4_ARTIFACTS) <= active_set,
        "Phase 4 semantic contracts reference non-active intents: "
        f"{sorted(set(PHASE4_ARTIFACTS) - active_set)}",
    )

    with tempfile.TemporaryDirectory(prefix="gamenet-intent-semantics-") as directory:
        temp_root = Path(directory)
        inventory = configure_inventory(
            repo_root,
            temp_root / "configured-repository",
            {
                "CMAKE_BUILD_TYPE": "Debug",
                "GAMENET_BUILD_BENCHMARKS": "ON",
                "GAMENET_BUILD_FUZZING": "OFF",
                "GAMENET_BUILD_TESTING": "ON",
                "GAMENET_ENABLE_EXPERIMENTAL": "OFF",
                "GAMENET_ENABLE_TLS": "OFF",
            },
        )

        validate_dependency_direction(inventory)

        for relative_path in active_paths:
            is_phase4 = relative_path in PHASE4_ARTIFACTS
            validate_intent(
                repo_root / relative_path,
                repo_root,
                inventory,
                ci_scripts,
                fuzz_cmake,
                CONCRETE_ACTIVE_ARTIFACTS.get(relative_path),
                frozen_core=relative_path in FROZEN_CORE_ACTIVE_INTENTS,
                require_provenance=is_phase4,
                require_verification_path=is_phase4,
            )

        run_negative_fixtures(temp_root)
        run_dependency_direction_fixtures(temp_root)

    pipeline_metadata, _ = parse_front_matter(
        repo_root / "intents/usecases/game_server_pipeline_demo.intent.md"
    )
    stale_pipeline_library = "GameNet::" + "game"
    require(
        pipeline_metadata["target"] != stale_pipeline_library,
        "the non-installed Pipeline example must not claim the deferred game library alias",
    )
    print(
        f"validated {len(active_paths)} active intent targets, "
        f"{explicit_verification_path_count} explicit verification paths, "
        f"{len(FROZEN_CORE_ACTIVE_INTENTS)} frozen Core metadata exemptions, "
        "and configured production dependency direction"
    )


if __name__ == "__main__":
    main()
