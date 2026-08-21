import json
import sys
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing IOE-X1 fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    root_cmake = repo_root / "CMakeLists.txt"
    experimental_cmake = repo_root / "src/experimental/io_uring/CMakeLists.txt"
    engine_header = repo_root / "src/experimental/io_uring/IoUringCompletionEngine.h"
    engine_source = repo_root / "src/experimental/io_uring/IoUringCompletionEngine.cc"
    pump_header = repo_root / "src/experimental/io_uring/IoUringEventLoopPump.h"
    pump_source = repo_root / "src/experimental/io_uring/IoUringEventLoopPump.cc"
    driver_header = (
        repo_root / "src/experimental/io_uring/IoUringTcpConnectionDriver.h"
    )
    driver_source = (
        repo_root / "src/experimental/io_uring/IoUringTcpConnectionDriver.cc"
    )
    hub_header = (
        repo_root / "src/experimental/io_uring/IoUringTcpConnectionHub.h"
    )
    hub_source = (
        repo_root / "src/experimental/io_uring/IoUringTcpConnectionHub.cc"
    )
    adapter_header = (
        repo_root / "src/experimental/io_uring/IoUringTcpConnectionAdapter.h"
    )
    adapter_source = (
        repo_root / "src/experimental/io_uring/IoUringTcpConnectionAdapter.cc"
    )
    lifecycle_registry = (
        repo_root / "src/core/net/detail/EventLoopLifecycleRegistry.h"
    )
    event_loop_source = repo_root / "src/core/net/EventLoop.cc"
    core_cmake = repo_root / "src/core/CMakeLists.txt"
    adapter = repo_root / "src/core/net/detail/PollerIoEngineAdapter.cc"
    tests_cmake = repo_root / "tests/CMakeLists.txt"
    contract = repo_root / "tests/contract/io_engine/test_io_uring_completion_engine.cpp"
    pump_contract = (
        repo_root / "tests/contract/io_engine/test_io_uring_event_loop_pump.cpp"
    )
    driver_contract = (
        repo_root
        / "tests/contract/io_engine/test_io_uring_tcp_connection_driver.cpp"
    )
    hub_contract = (
        repo_root
        / "tests/contract/io_engine/test_io_uring_tcp_connection_hub.cpp"
    )
    hub_capacity_contract = (
        repo_root
        / "tests/contract/io_engine/test_io_uring_tcp_connection_hub_capacity.cpp"
    )
    listener_contract = (
        repo_root
        / "tests/contract/io_engine/test_io_uring_tcp_listener.cpp"
    )
    adapter_contract = (
        repo_root
        / "tests/contract/io_engine/test_io_uring_tcp_connection_adapter.cpp"
    )
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    benchmark = repo_root / "benchmarks" / "io_uring" / "one_shot.cpp"
    benchmark_validator = repo_root / "tools" / "validate_io_uring_benchmark.py"
    shared_hub_benchmark = (
        repo_root / "benchmarks" / "io_uring" / "shared_tcp_hub.cpp"
    )
    shared_hub_validator = (
        repo_root / "tools" / "validate_io_uring_shared_hub_benchmark.py"
    )
    listener_comparison_benchmark = (
        repo_root / "benchmarks" / "io_uring" / "listener_comparison.cpp"
    )
    listener_comparison_validator = (
        repo_root / "tools" / "validate_ioe_x10_listener_comparison.py"
    )
    listener_comparison_runner = (
        repo_root / "tools" / "run_io_uring_listener_comparison.py"
    )
    benchmark_docs = repo_root / "docs" / "development" / "io_uring_benchmark.md"
    intent = repo_root / "intents/architecture/io_engine.intent.md"
    thread_rules = repo_root / "rules/thread_affinity_rules.md"
    ownership_rules = repo_root / "rules/ownership_rules.md"
    testing_rules = repo_root / "rules/testing_rules.md"
    workflow = repo_root / ".github/workflows/ci.yml"
    platform_docs = repo_root / "docs/development/platform_support.md"

    for path in (
        experimental_cmake,
        engine_header,
        engine_source,
        pump_header,
        pump_source,
        driver_header,
        driver_source,
        hub_header,
        hub_source,
        adapter_header,
        adapter_source,
        contract,
        pump_contract,
        driver_contract,
        hub_contract,
        hub_capacity_contract,
        listener_contract,
        adapter_contract,
        benchmark,
        benchmark_validator,
        shared_hub_benchmark,
        shared_hub_validator,
        listener_comparison_benchmark,
        listener_comparison_validator,
        listener_comparison_runner,
        benchmark_docs,
    ):
        assert path.is_file(), f"missing IOE-X1 artifact: {path}"

    root_text = root_cmake.read_text(encoding="utf-8")
    require(root_text, "GAMENET_ENABLE_EXPERIMENTAL requires Linux", root_cmake)
    require(root_text, "add_subdirectory(src/experimental/io_uring)", root_cmake)
    assert root_text.index("if(GAMENET_ENABLE_EXPERIMENTAL)") < root_text.index(
        "add_subdirectory(src/experimental/io_uring)"
    )

    cmake_text = experimental_cmake.read_text(encoding="utf-8")
    require(cmake_text, "add_library(gamenet_experimental_io_uring STATIC", experimental_cmake)
    require(cmake_text, "add_library(GameNet::experimental ALIAS", experimental_cmake)
    require(cmake_text, "IoUringTcpConnectionHub.cc", experimental_cmake)
    require(cmake_text, "IoUringTcpConnectionAdapter.cc", experimental_cmake)
    require(cmake_text, "gamenet_configure_sanitizers", experimental_cmake)
    assert "install(" not in cmake_text, "IOE-X1 target must remain non-installed"

    combined = engine_header.read_text(encoding="utf-8") + engine_source.read_text(encoding="utf-8")
    for fragment in (
        "__NR_io_uring_setup",
        "__NR_io_uring_enter",
        "__NR_io_uring_register",
        "IORING_OP_ACCEPT",
        "IORING_OP_RECV",
        "IORING_OP_SEND",
        "IORING_OP_ASYNC_CANCEL",
        "SubmissionQueueFull",
        "maxOperations",
        "maxOwnedBytes",
        "generation",
        "noticePending",
        "beginQuiesce",
        "IoUringShutdownResult",
    ):
        require(combined, fragment, engine_source)

    for forbidden in (
        "IORING_ACCEPT_MULTISHOT",
        "IORING_RECV_MULTISHOT",
        "IOSQE_BUFFER_SELECT",
        "IORING_REGISTER_BUFFERS",
        "IORING_REGISTER_FILES",
        "IORING_OP_PROVIDE_BUFFERS",
        "IORING_OP_SEND_ZC",
        "IORING_SETUP_SQPOLL",
        "IOSQE_IO_LINK",
        "IOSQE_IO_HARDLINK",
    ):
        assert forbidden not in combined, f"IOE-X1 advanced feature escaped scope: {forbidden}"

    core_text = core_cmake.read_text(encoding="utf-8")
    adapter_text = adapter.read_text(encoding="utf-8")
    assert "IoUring" not in core_text
    assert "IoUring" not in adapter_text
    require(adapter_text, '#include "EpollReadinessPort.h"', adapter)

    pump_text = pump_header.read_text(encoding="utf-8") + pump_source.read_text(
        encoding="utf-8"
    )
    for fragment in (
        "IoUringEventLoopPump",
        "completionDescriptor",
        "maxNoticesPerTurn",
        "attachQuiesceParticipant",
        "engine_.wait(std::chrono::milliseconds::zero())",
        "IoUringEventLoopPumpStopResult::DrainedAfterFailure",
        "consumerFailures",
    ):
        require(pump_text, fragment, pump_source)
    for forbidden in (
        '"gamenet/core/net/TcpConnection.h"',
        "queueInLoop",
        "runInLoop",
        "std::thread",
        "IORING_RECV_MULTISHOT",
        "IORING_OP_SEND_ZC",
    ):
        assert forbidden not in pump_text, f"IOE-X2 scope escape: {forbidden}"
    require(pump_text, "std::terminate()", pump_source)
    require(pump_text, "stopFuture_.wait_for", pump_source)
    require(pump_text, "rollbackInitialization", pump_source)
    assert "engine_.shutdown(std::chrono::milliseconds(250))" not in pump_text
    require(
        pump_contract.read_text(encoding="utf-8"),
        "testActiveDestructionFailsFastWithoutOwnerWait",
        pump_contract,
    )
    require(
        pump_contract.read_text(encoding="utf-8"),
        "WEXITSTATUS(status) == 86",
        pump_contract,
    )
    require(
        pump_contract.read_text(encoding="utf-8"),
        "testRegistrationFailureRollsBackEngine",
        pump_contract,
    )
    require(engine_source.read_text(encoding="utf-8"), "std::terminate()", engine_source)
    require(
        contract.read_text(encoding="utf-8"),
        "testForeignThreadDestructionFailsFast",
        contract,
    )
    assert "shutdown(std::chrono::milliseconds(250))" not in engine_source.read_text(
        encoding="utf-8"
    )

    driver_text = driver_header.read_text(encoding="utf-8") + driver_source.read_text(
        encoding="utf-8"
    )
    for fragment in (
        "IoUringTcpConnectionDriver",
        "maxPendingSendBytes",
        "maxPendingSendSegments",
        "maxSendBytesPerOperation",
        "IoUringTcpDriverCloseReason::EventLoopQuiescing",
        "receiveIdentity_",
        "sendIdentity_",
        "handlePumpStopped",
    ):
        require(driver_text, fragment, driver_source)
    require(pump_text, "StoppedConsumer", pump_source)
    for forbidden in (
        '"gamenet/core/net/TcpConnection.h"',
        "queueInLoop",
        "runInLoop",
        "std::thread",
        "IORING_RECV_MULTISHOT",
        "IORING_OP_SEND_ZC",
        "IORING_REGISTER_BUFFERS",
        "IORING_REGISTER_FILES",
    ):
        assert forbidden not in driver_text, f"IOE-X3 scope escape: {forbidden}"

    hub_text = hub_header.read_text(encoding="utf-8") + hub_source.read_text(
        encoding="utf-8"
    )
    for fragment in (
        "IoUringTcpConnectionHub",
        "IoUringTcpConnectionIdentity",
        "maxConnections",
        "maxPendingAccepts",
        "IoUringTcpHubListenOutcome",
        "IoUringTcpHubListenerStopSummary",
        "stopListening",
        "maxTotalPendingSendBytes",
        "operationRoutes_",
        "nextGeneration",
        "activeOperationRoutes_",
        "driveMaintenance",
        "handlePumpStopped",
    ):
        require(hub_text, fragment, hub_source)
    for forbidden in (
        '"gamenet/core/net/TcpConnection.h"',
        "queueInLoop",
        "runInLoop",
        "std::thread",
        "IORING_RECV_MULTISHOT",
        "IORING_OP_SEND_ZC",
        "IORING_REGISTER_BUFFERS",
        "IORING_REGISTER_FILES",
        "IORING_SETUP_SQPOLL",
    ):
        assert forbidden not in hub_text, f"IOE-X4 scope escape: {forbidden}"
    require(hub_text, "OutputProgressConsumer", hub_source)
    require(hub_text, "closeNativeError", hub_source)
    require(hub_text, "assertOwnerObservation", hub_source)
    hub_contract_text = hub_contract.read_text(encoding="utf-8")
    for observation in (
        "hub.listening()",
        "hub.listenerMetrics()",
        "hub.phase()",
        "hub.metrics()",
    ):
        require(hub_contract_text, observation, hub_contract)

    adapter_text = adapter_header.read_text(encoding="utf-8") + adapter_source.read_text(
        encoding="utf-8"
    )
    for fragment in (
        "IoUringTcpConnectionAdapter",
        "TcpSendResult",
        "TcpConnectionCloseInfo",
        "lowWaterMarkBytes",
        "highWaterMarkBytes",
        "hardLimitBytes",
        "maxPendingCommands",
        "maxCommandsPerTurn",
        "readingPausedByBackpressure",
        "tryShutdown",
        "tryForceClose",
        "GracefulShutdown",
        "AdapterCommandState",
        "EventLoopLifecycleRegistry::attach",
        "SchedulingQueueFull",
        "foreignSendAdmissions",
        "foreignLifecycleAdmissions",
        "cancelledCommandBytes",
        "stopFuture",
        "IoUringTcpHubCloseReason::Destroyed",
        "observer = nullptr",
    ):
        require(adapter_text, fragment, adapter_source)
    for forbidden in (
        "std::thread",
        "queueInLoop(",
        "IORING_RECV_MULTISHOT",
        "IORING_OP_SEND_ZC",
        "IORING_REGISTER_BUFFERS",
        "IORING_REGISTER_FILES",
        "IORING_SETUP_SQPOLL",
    ):
        assert forbidden not in adapter_text, f"IOE-X6/X7/X8 scope escape: {forbidden}"

    registry_text = lifecycle_registry.read_text(encoding="utf-8")
    event_loop_text = event_loop_source.read_text(encoding="utf-8")
    require(registry_text, "attachQuiesceParticipant", lifecycle_registry)
    require(event_loop_text, "notifyOnQuiesce", event_loop_source)
    require(event_loop_text, "quiesceHead", event_loop_source)

    tests_text = tests_cmake.read_text(encoding="utf-8")
    require(tests_text, "if(GAMENET_ENABLE_EXPERIMENTAL)", tests_cmake)
    require(tests_text, "gamenet_io_uring_contract", tests_cmake)
    require(tests_text, "gamenet_io_uring_event_loop_pump_contract", tests_cmake)
    require(tests_text, "gamenet_io_uring_tcp_connection_driver_contract", tests_cmake)
    require(tests_text, "gamenet_io_uring_tcp_connection_hub_contract", tests_cmake)
    require(
        tests_text,
        "gamenet_io_uring_tcp_connection_hub_capacity_contract",
        tests_cmake,
    )
    require(
        tests_text,
        "gamenet_io_uring_tcp_listener_contract",
        tests_cmake,
    )
    require(
        tests_text,
        "gamenet_io_uring_tcp_connection_adapter_contract",
        tests_cmake,
    )
    require(tests_text, "gamenet_io_uring_contracts", tests_cmake)
    require(tests_text, "test_io_uring_event_loop_pump.cpp", tests_cmake)
    require(tests_text, "test_io_uring_tcp_connection_driver.cpp", tests_cmake)
    require(tests_text, "test_io_uring_tcp_connection_hub.cpp", tests_cmake)
    require(
        tests_text,
        "test_io_uring_tcp_connection_hub_capacity.cpp",
        tests_cmake,
    )
    require(
        tests_text,
        "test_io_uring_tcp_listener.cpp",
        tests_cmake,
    )
    require(
        tests_text,
        "test_io_uring_tcp_connection_adapter.cpp",
        tests_cmake,
    )
    require(tests_text, "contract.io_engine.test_io_uring_completion_engine", tests_cmake)
    require(tests_text, "GameNet::experimental", tests_cmake)
    require(tests_text, "experimental;threading;lifecycle", tests_cmake)

    contract_text = contract.read_text(encoding="utf-8")
    for fragment in (
        "testFiniteSqRejectsWithoutFallback",
        "testOneShotAcceptRecvSend",
        "testTerminalNoticeRetainsOperationSlotGeneration",
        "testCancelLeaseAndFinalDrain",
        "testForeignThreadMutationRejected",
        "SubmissionQueueFull",
        "IoUringCompletionStatus::Cancelled",
        "crossDomain",
    ):
        require(contract_text, fragment, contract)

    pump_contract_text = pump_contract.read_text(encoding="utf-8")
    for fragment in (
        "int main()",
        "maxNoticesPerTurn = 1",
        "loop.quit()",
        "IoUringCompletionStatus::Cancelled",
        "DrainedAfterFailure",
        "observedPendingLease.expired()",
        "continuationSignals",
        "pendingCancelCompletions",
    ):
        require(pump_contract_text, fragment, pump_contract)

    driver_contract_text = driver_contract.read_text(encoding="utf-8")
    for fragment in (
        "testFiniteSendAndReentrantPauseResume",
        "testResumeWaitsForCancelledReceiveTerminal",
        "testEventLoopQuitCancelsPendingReceiveAndRejectsForeignMutation",
        "testExplicitCloseAccountsAcceptedSendAndPendingReceive",
        "testMessageFailureClosesWithoutEscapingPump",
        "maxActiveReceives == 1",
        "IoUringTcpDriverSendResult::ByteLimit",
        "IoUringTcpDriverSendResult::SegmentLimit",
        "EventLoopQuiescing",
        "socketCloseCount == 1",
        "AF_INET",
    ):
        require(driver_contract_text, fragment, driver_contract)
    assert "socketpair(" not in driver_contract_text

    hub_contract_text = hub_contract.read_text(encoding="utf-8")
    for fragment in (
        "testSharedPumpIsolationGenerationAndReentrantReuse",
        "testCapacityForeignMutationAndAggregateEventLoopQuit",
        "ConnectionByteLimit",
        "ConnectionSegmentLimit",
        "HubByteLimit",
        "StaleConnection",
        "EventLoopQuiescing",
        "maxActiveConnections == 2",
        "AF_INET",
    ):
        require(hub_contract_text, fragment, hub_contract)
    assert "socketpair(" not in hub_contract_text

    hub_capacity_text = hub_capacity_contract.read_text(encoding="utf-8")
    for fragment in (
        "kInitialConnections = 256",
        "kReplacementConnections = 64",
        "testFixed256RouteChurnAndPostReplacementProgress",
        "IoUringTcpHubSendResult::HubByteLimit",
        "maxActiveConnections == 256",
        "connectionsAccepted == 320",
        "activeOperations == 0",
        "AF_INET",
    ):
        require(hub_capacity_text, fragment, hub_capacity_contract)
    assert "socketpair(" not in hub_capacity_text

    listener_contract_text = listener_contract.read_text(encoding="utf-8")
    for fragment in (
        "testProductionAndCompletionListenerEchoAndStop",
        "testListenerBurstCapacityRecoveryAndGenerationChurn",
        "testListenerFirstArmOperationPressureIsTyped",
        "testListenerFactoryReentryStopsAdmission",
        "testListenerFactoryFailuresFailClosed",
        "testListenerOwnerQuitDrains",
        "IoUringTcpHubListenResult::EngineRejected",
        "connectionLimitRejections == 1",
        "maxActiveAccepts == 2",
        "IoUringTcpHubListenerCloseReason::EventLoopQuiescing",
        "activeOperationRoutes == 0",
        "TcpServerStopOutcome::Drained",
        "AF_INET",
    ):
        require(listener_contract_text, fragment, listener_contract)
    assert "socketpair(" not in listener_contract_text

    adapter_contract_text = adapter_contract.read_text(encoding="utf-8")
    for fragment in (
        "testProductionAndAdapterCommonSemantics",
        "testObserverDestructionRetainsPhysicalObligation",
        "testProductionAndAdapterGracefulDrainAndHalfClose",
        "testAdapterGracefulReasonSurvivesForcedEscalation",
        "testAdapterGracefulPeerResetRetiresPendingWork",
        "testAdapterGracefulCallbackFailureIsContained",
        "testAdapterGracefulOwnerQuitFinalDrains",
        "testAdapterForeignMailboxSaturationRecoversInOrder",
        "testAdapterCommandOptionsRejectInvalidBounds",
        "testAdapterConcurrentForeignTerminalFirstReasonWins",
        "kPayloadBytes = 4U * 1024U * 1024U",
        "TcpSendResult::Overloaded",
        "readingPausedByBackpressure",
        "TcpConnectionCloseReason::ForcedShutdown",
        "TcpConnectionCloseReason::GracefulShutdown",
        "writeHalfCloses == 1",
        "TcpSendResult::SchedulingQueueFull",
        "PostResult::QueueFull",
        "foreignSendAdmissions == 3",
        "foreignLifecycleAdmissions",
        "cancelledCommandBytes",
        "std::barrier",
        "adapter.reset()",
        "activeOperations == 0",
        "AF_INET",
    ):
        require(adapter_contract_text, fragment, adapter_contract)
    assert "socketpair(" not in adapter_contract_text

    benchmark_cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    require(benchmark_cmake_text, "if(GAMENET_ENABLE_EXPERIMENTAL)", benchmark_cmake)
    require(
        benchmark_cmake_text,
        "add_executable(gamenet_io_uring_one_shot_benchmark",
        benchmark_cmake,
    )
    require(
        benchmark_cmake_text,
        "add_executable(gamenet_io_uring_shared_tcp_hub_benchmark",
        benchmark_cmake,
    )
    require(
        benchmark_cmake_text,
        "add_executable(gamenet_ioe_x10_listener_comparison_benchmark",
        benchmark_cmake,
    )
    require(benchmark_cmake_text, "GameNet::experimental", benchmark_cmake)
    assert "add_test(" not in benchmark_cmake_text
    assert "install(" not in benchmark_cmake_text

    benchmark_text = benchmark.read_text(encoding="utf-8")
    for fragment in (
        "gamenet.io_uring_one_shot_benchmark.v1",
        "IoUringOperationKind::Send",
        "IoUringOperationKind::Receive",
        "operations_accepted",
        "terminal_notices",
        "cross_domain_fallbacks",
        "owned_bytes",
        "p999_latency_us",
        "IoUringShutdownResult::Drained",
    ):
        require(benchmark_text, fragment, benchmark)

    validator_text = benchmark_validator.read_text(encoding="utf-8")
    require(
        validator_text,
        'SCHEMA = "gamenet.io_uring_one_shot_benchmark.v1"',
        benchmark_validator,
    )
    require(
        validator_text,
        "operations accepted must equal twice round trips",
        benchmark_validator,
    )
    require(
        validator_text,
        "io_uring benchmark retained residual state",
        benchmark_validator,
    )
    require(validator_text, "io_uring benchmark used a fallback", benchmark_validator)

    shared_hub_benchmark_text = shared_hub_benchmark.read_text(encoding="utf-8")
    for fragment in (
        "gamenet.io_uring_shared_tcp_hub_benchmark.v1",
        "IoUringTcpConnectionHub",
        "completed_round_trips",
        "working_set_bytes_per_connection",
        "active_operation_routes",
        "engine_owned_bytes",
    ):
        require(shared_hub_benchmark_text, fragment, shared_hub_benchmark)

    shared_hub_validator_text = shared_hub_validator.read_text(encoding="utf-8")
    require(
        shared_hub_validator_text,
        'SCHEMA = "gamenet.io_uring_shared_tcp_hub_benchmark.v1"',
        shared_hub_validator,
    )
    require(shared_hub_validator_text, "max_active_connections", shared_hub_validator)
    require(shared_hub_validator_text, "engine_owned_bytes", shared_hub_validator)

    listener_comparison_text = listener_comparison_benchmark.read_text(encoding="utf-8")
    for fragment in (
        "gamenet.ioe_x10_listener_comparison.v1",
        "kActiveRoutes = 256",
        "kMaxPendingAccepts = 32",
        "kChurnWaves = 4",
        "kReplacementsPerWave = 64",
        "kRoundTripsPerRoutePerWave = 100",
        "kPayloadBytes = 64",
        "maxActiveOperations",
        "maxEngineOwnedBytes",
        "capacity_recovered",
        "fd_residue",
    ):
        require(listener_comparison_text, fragment, listener_comparison_benchmark)
    listener_validator_text = listener_comparison_validator.read_text(encoding="utf-8")
    require(
        listener_validator_text,
        'SCHEMA = "gamenet.ioe_x10_listener_comparison.v1"',
        listener_comparison_validator,
    )
    require(listener_validator_text, '"active_routes": 256', listener_comparison_validator)
    require(listener_validator_text, "epoll must not synthesize", listener_comparison_validator)
    listener_runner_text = listener_comparison_runner.read_text(encoding="utf-8")
    require(
        listener_runner_text,
        'SCHEMA = "gamenet.ioe_x10_listener_evidence.v1"',
        listener_comparison_runner,
    )
    require(listener_runner_text, "SAMPLES_PER_BACKEND = 5", listener_comparison_runner)
    require(listener_runner_text, "WARMUPS_PER_BACKEND = 1", listener_comparison_runner)
    require(listener_runner_text, 'decision = "PROMOTE"', listener_comparison_runner)

    sys.path.insert(0, str(repo_root / "tools"))
    import validate_io_uring_benchmark as benchmark_contract
    import validate_ioe_x10_listener_comparison as listener_comparison_contract

    def listener_document(backend: str) -> dict:
        completion = backend == "io_uring"
        return {
            "schema": "gamenet.ioe_x10_listener_comparison.v1",
            "status": "ok",
            "backend": backend,
            "build_type": "Release",
            "parameters": dict(listener_comparison_contract.PARAMETERS),
            "measurements": {
                "connect_completions": 516,
                "connections_admitted": 512,
                "echo_completions": 128000,
                "close_completions": 512,
                "connect_completion_rate": 1.0,
                "echo_completion_rate": 1.0,
                "close_completion_rate": 1.0,
                "connects_per_second": 1000.0,
                "round_trips_per_second": 1000.0,
                "throughput_mib_per_second": 10.0,
                "p50_latency_us": 10,
                "p99_latency_us": 20,
                "p999_latency_us": 30,
                "fd_baseline": 4,
                "fd_high_water": 520,
                "fd_final": 4,
                "fd_residue": 0,
                "max_active_routes": 256,
                "max_active_accepts": 32 if completion else None,
                "max_active_recvs": 256 if completion else None,
                "max_active_sends": 256 if completion else None,
                "max_active_operations": 544 if completion else None,
                "max_pending_send_bytes": 16384,
                "max_engine_owned_bytes": 32768 if completion else None,
                "rss_before_bytes": 1000000,
                "rss_high_water_bytes": 2000000,
                "rss_after_bytes": 1500000,
                "capacity_rejections": 4,
                "sq_rejections": 0 if completion else None,
                "capacity_recovered": True,
                "max_capacity_recovery_milliseconds": 2.0,
                "listener_shutdown_milliseconds": 1.0,
                "server_shutdown_milliseconds": 1.0,
                "listener_closed": True,
                "final_active_routes": 0,
                "final_active_accepts": 0 if completion else None,
                "final_active_operations": 0 if completion else None,
                "final_ready_notices": 0 if completion else None,
                "final_pending_send_bytes": 0,
                "final_engine_owned_bytes": 0 if completion else None,
            },
        }

    for backend in ("epoll", "io_uring"):
        listener_comparison_contract.validate_document(
            listener_document(backend), expected_backend=backend
        )
    invalid_listener = listener_document("io_uring")
    invalid_listener["measurements"]["final_active_operations"] = 1
    try:
        listener_comparison_contract.validate_document(invalid_listener)
    except listener_comparison_contract.ListenerComparisonValidationError:
        pass
    else:
        raise AssertionError("IOE-X10 validator accepted residual operation state")
    fake_epoll = listener_document("epoll")
    fake_epoll["measurements"]["max_active_accepts"] = 32
    try:
        listener_comparison_contract.validate_document(fake_epoll)
    except listener_comparison_contract.ListenerComparisonValidationError:
        pass
    else:
        raise AssertionError("IOE-X10 validator accepted fake epoll completion data")

    document = {
        "schema": "gamenet.io_uring_one_shot_benchmark.v1",
        "status": "ok",
        "build_type": "Release",
        "parameters": {"round_trips": 1000, "payload_bytes": 256, "depth": 32},
        "measurements": {
            "elapsed_seconds": 0.01,
            "messages_per_second": 100000.0,
            "operations_per_second": 200000.0,
            "throughput_mib_per_second": 48.828125,
            "p50_latency_us": 10,
            "p99_latency_us": 20,
            "p999_latency_us": 30,
            "working_set_before_bytes": 1000000,
            "working_set_after_bytes": 1004096,
            "working_set_delta_bytes": 4096,
            "shutdown_milliseconds": 0.1,
            "operations_accepted": 2000,
            "terminal_notices": 2000,
            "sq_full_rejections": 0,
            "cross_domain_fallbacks": 0,
            "active_operations": 0,
            "ready_notices": 0,
            "owned_bytes": 0,
        },
    }
    benchmark_contract.validate_document(document, require_release=True)
    invalid = json.loads(json.dumps(document))
    invalid["measurements"]["terminal_notices"] = 1999
    try:
        benchmark_contract.validate_document(invalid, require_release=True)
    except benchmark_contract.IoUringBenchmarkValidationError:
        pass
    else:
        raise AssertionError("io_uring validator accepted missing terminal notice")

    docs_text = benchmark_docs.read_text(encoding="utf-8")
    require(docs_text, "gamenet_io_uring_one_shot_benchmark", benchmark_docs)
    require(docs_text, "validate_io_uring_benchmark.py", benchmark_docs)
    require(docs_text, "gamenet_io_uring_shared_tcp_hub_benchmark", benchmark_docs)
    require(docs_text, "validate_io_uring_shared_hub_benchmark.py", benchmark_docs)
    require(docs_text, "IOE-X5 decision: `PROMOTE`", benchmark_docs)
    require(docs_text, "directional", benchmark_docs)

    intent_text = intent.read_text(encoding="utf-8")
    require(intent_text, "IOE-X1 authorizes one experimental Linux completion vertical slice", intent)
    require(intent_text, "IOE-X2 authorizes one source-private EventLoop-driven completion pump", intent)
    require(intent_text, "IOE-X3 authorizes one experimental single-connection Completion TCP driver", intent)
    require(intent_text, "IOE-X4 authorizes one experimental shared-Pump connection hub", intent)
    require(intent_text, "IOE-X5 authorizes capacity, churn, and directional measurement", intent)
    require(intent_text, "IOE-X6 authorizes one non-installed TCP semantic adapter", intent)
    require(intent_text, "IOE-X7 authorizes that missing graceful-drain", intent)
    require(intent_text, "IOE-X8 authorizes bounded cross-thread command admission", intent)
    require(intent_text, "IOE-X9 authorizes one source-private listener", intent)
    require(intent_text, "tests/contract/io_engine/test_io_uring_completion_engine.cpp", intent)
    require(intent_text, "tests/contract/io_engine/test_io_uring_event_loop_pump.cpp", intent)
    require(intent_text, "tests/contract/io_engine/test_io_uring_tcp_connection_driver.cpp", intent)
    require(intent_text, "tests/contract/io_engine/test_io_uring_tcp_connection_hub.cpp", intent)
    require(
        intent_text,
        "tests/contract/io_engine/test_io_uring_tcp_connection_hub_capacity.cpp",
        intent,
    )
    require(
        intent_text,
        "tests/contract/io_engine/test_io_uring_tcp_connection_adapter.cpp",
        intent,
    )
    require(
        intent_text,
        "tests/contract/io_engine/test_io_uring_tcp_listener.cpp",
        intent,
    )
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X1's raw io_uring Engine", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X2's non-installed completion pump", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X3 single-connection driver", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X4 connection Hub", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X5 capacity/churn", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X6 semantic adapter", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X7 graceful request", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X8 permits only Adapter", thread_rules)
    require(thread_rules.read_text(encoding="utf-8"), "IOE-X9 listener construction", thread_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X1 experimental target owns", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X2 pump owns", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X3 driver uniquely owns", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X4 Hub uniquely owns", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X5 capacity fixture", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X6 adapter borrows", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X7 graceful shutdown", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X8 foreign Send admission", ownership_rules)
    require(ownership_rules.read_text(encoding="utf-8"), "IOE-X9 `listen` transfers", ownership_rules)
    require(testing_rules.read_text(encoding="utf-8"), "real Linux io_uring fd", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X2 contract", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X3 contract", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X4 contract", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X5 capacity contract", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X6 cross-backend contract", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X7 graceful case", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X8 cross-thread contract", testing_rules)
    require(testing_rules.read_text(encoding="utf-8"), "IOE-X9 listener contract", testing_rules)

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "test_io_uring_completion_engine_contract.py", workflow)
    require(workflow_text, "GAMENET_ENABLE_EXPERIMENTAL=ON", workflow)
    require(workflow_text, "contract.io_engine.test_io_uring_", workflow)
    require(workflow_text, "event_loop_pump", workflow)
    require(workflow_text, "tcp_connection_driver", workflow)
    require(workflow_text, "tcp_connection_hub", workflow)
    require(workflow_text, "tcp_connection_hub_capacity", workflow)
    require(workflow_text, "tcp_listener", workflow)
    require(workflow_text, "tcp_connection_adapter", workflow)
    require(
        platform_docs.read_text(encoding="utf-8"),
        "IOE-X1/X2/X3/X4/X5/X6/X7/X8/X9 io_uring",
        platform_docs,
    )

    print("IOE-X1/X2/X3/X4/X5/X6/X7/X8/X9 Engine, Pump, driver, Hub, capacity, adapter, graceful, cross-thread, and listener contracts verified")


if __name__ == "__main__":
    main()
