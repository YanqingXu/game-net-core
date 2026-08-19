from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing IOCP data-path fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    spec = repo_root / "docs" / "superpowers" / "specs" / "2026-07-07-windows-iocp-data-path-design.md"
    plan = repo_root / "docs" / "superpowers" / "plans" / "2026-07-07-windows-iocp-data-path.md"
    milestone = repo_root / "docs" / "development" / "windows_iocp_milestone.md"
    operation = repo_root / "include" / "gamenet" / "core" / "net" / "platform" / "IocpOperation.h"
    completion_port = repo_root / "src" / "core" / "net" / "detail" / "CompletionPort.h"
    operation_state = repo_root / "src" / "core" / "net" / "detail" / "IocpOperationState.h"
    socket_ops = repo_root / "include" / "gamenet" / "core" / "net" / "platform" / "IocpSocketOps.h"
    socket_ops_source = repo_root / "src" / "core" / "net" / "platform" / "IocpSocketOps_win.cc"
    sockets_win = repo_root / "src" / "core" / "net" / "platform" / "SocketsOps_win.cc"
    acceptor_header = repo_root / "include" / "gamenet" / "core" / "net" / "Acceptor.h"
    acceptor_source = repo_root / "src" / "core" / "net" / "Acceptor.cc"
    connector_header = repo_root / "include" / "gamenet" / "core" / "net" / "Connector.h"
    connector_source = repo_root / "src" / "core" / "net" / "Connector.cc"
    tcp_client_source = repo_root / "src" / "core" / "net" / "TcpClient.cc"
    tcp_connection_header = repo_root / "include" / "gamenet" / "core" / "net" / "TcpConnection.h"
    tcp_connection_source = repo_root / "src" / "core" / "net" / "TcpConnection.cc"
    event_loop_source = repo_root / "src" / "core" / "net" / "EventLoop.cc"
    channel_header = repo_root / "include" / "gamenet" / "core" / "net" / "Channel.h"
    channel_source = repo_root / "src" / "core" / "net" / "Channel.cc"
    io_engine_adapter = (
        repo_root / "src" / "core" / "net" / "detail" / "PollerIoEngineAdapter.cc"
    )
    iocp_poller_access = (
        repo_root / "src" / "core" / "net" / "detail" / "IocpPollerAccess.h"
    )
    iocp_test_harness = (
        repo_root
        / "src"
        / "core"
        / "net"
        / "detail"
        / "EventLoopIocpAssociationHarness.h"
    )
    tcp_transport = repo_root / "src" / "core" / "net" / "platform" / "IocpTcpTransport.h"
    tcp_transport_source = repo_root / "src" / "core" / "net" / "platform" / "IocpTcpTransport_win.cc"
    sync_error_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_iocp_sync_error.cpp"
    )
    segmented_write_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_iocp_segmented_write.cpp"
    )
    partial_write_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_iocp_partial_write.cpp"
    )
    read_storage_test = (
        repo_root
        / "tests"
        / "contract"
        / "tcp_connection"
        / "test_tcp_connection_iocp_read_storage.cpp"
    )
    acceptor_pool_test = (
        repo_root
        / "tests"
        / "contract"
        / "acceptor"
        / "test_acceptor_iocp_pool.cpp"
    )
    poller_contract_test = (
        repo_root
        / "tests"
        / "contract"
        / "poller"
        / "test_poller_contract.cpp"
    )
    completion_engine_test = (
        repo_root
        / "tests"
        / "contract"
        / "io_engine"
        / "test_completion_engine.cpp"
    )
    accept_connect_drain_test = (
        repo_root
        / "tests"
        / "integration"
        / "tcp"
        / "test_iocp_accept_connect_quit_completion_drain.cpp"
    )
    quit_completion_drain_test = (
        repo_root
        / "tests"
        / "integration"
        / "tcp"
        / "test_iocp_quit_completion_drain.cpp"
    )
    lifecycle_hub_test = (
        repo_root
        / "tests"
        / "contract"
        / "event_loop"
        / "test_event_loop_lifecycle_hub.cpp"
    )
    poller_header = repo_root / "include" / "gamenet" / "core" / "net" / "poller" / "IocpPoller.h"
    poller_source = repo_root / "src" / "core" / "net" / "poller" / "IocpPoller.cc"
    wakeup_coalescing_test = (
        repo_root
        / "tests"
        / "contract"
        / "event_loop"
        / "test_event_loop_wakeup_coalescing.cpp"
    )
    core_cmake = repo_root / "src" / "core" / "CMakeLists.txt"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    require(spec.read_text(encoding="utf-8"), "loop-owned Windows IOCP operation layer", spec)
    require(plan.read_text(encoding="utf-8"), "WSARecv", plan)
    require(milestone.read_text(encoding="utf-8"), "windows-iocp-data-path-design.md", milestone)

    operation_text = operation.read_text(encoding="utf-8")
    require(operation_text, "enum class IocpOperationKind", operation)
    require(operation_text, "OVERLAPPED overlapped", operation)
    require(operation_text, "bytesTransferred", operation)
    require(operation_text, "Channel* channel", operation)
    require(operation_text, "shutdownObligation", operation)
    require(operation_text, "completionObserved", operation)
    assert "nextPublishedCompletion" not in operation_text, (
        "typed IOCP operations must not carry a fake-readiness publication link"
    )
    require(operation_text, "generation", operation)
    require(operation_text, "terminalGeneration", operation)
    require(operation_text, "terminalObserver", operation)
    require(operation_text, "observerSource", operation)
    require(operation_text, "observerRegistrationGeneration", operation)
    require(operation_text, "observerIdentityCaptured", operation)

    completion_port_text = completion_port.read_text(encoding="utf-8")
    require(completion_port_text, "struct CompletionOperationIdentity", completion_port)
    require(completion_port_text, "struct CompletionNotice", completion_port)
    require(completion_port_text, "CompletionTerminalStatus", completion_port)
    require(completion_port_text, "CompletionWaitResult", completion_port)

    operation_state_text = operation_state.read_text(encoding="utf-8")
    require(operation_state_text, "prepareIocpOperationSubmission", operation_state)
    require(operation_state_text, "commitIocpOperationSubmission", operation_state)
    require(operation_state_text, "rejectIocpOperationSubmission", operation_state)
    require(operation_state_text, "retireIocpOperationSubmission", operation_state)

    socket_ops_text = socket_ops.read_text(encoding="utf-8")
    require(socket_ops_text, "loadAcceptEx", socket_ops)
    require(socket_ops_text, "loadConnectEx", socket_ops)
    require(socket_ops_text, "SocketFd createOverlappedTcp(sa_family_t family);", socket_ops)
    require(socket_ops_text, "bool bindUnspecified", socket_ops)
    require(socket_ops_text, "bool updateAcceptContext", socket_ops)
    require(socket_ops_text, "bool updateConnectContext", socket_ops)

    socket_ops_source_text = socket_ops_source.read_text(encoding="utf-8")
    require(socket_ops_source_text, "WSASocketW", socket_ops_source)
    require(socket_ops_source_text, "WSA_FLAG_OVERLAPPED", socket_ops_source)
    require(socket_ops_source_text, "SIO_GET_EXTENSION_FUNCTION_POINTER", socket_ops_source)

    sockets_win_text = sockets_win.read_text(encoding="utf-8")
    require(sockets_win_text, "SocketFd createNonblocking(sa_family_t family)", sockets_win)

    acceptor_header_text = acceptor_header.read_text(encoding="utf-8")
    require(acceptor_header_text, "IocpAcceptState", acceptor_header)
    require(acceptor_header_text, "postAccept", acceptor_header)
    require(acceptor_header_text, "setIocpAcceptDepth", acceptor_header)
    require(acceptor_header_text, "IocpAcceptSlot", acceptor_header)
    require(acceptor_header_text, "fillAcceptPool", acceptor_header)

    acceptor_source_text = acceptor_source.read_text(encoding="utf-8")
    require(acceptor_source_text, "platform::loadAcceptEx", acceptor_source)
    require(acceptor_source_text, "platform::createOverlappedTcp", acceptor_source)
    require(acceptor_source_text, "platform::updateAcceptContext", acceptor_source)
    assert "platform::createOverlappedTcpOrDie" not in acceptor_source_text
    assert "platform::updateAcceptContextOrDie" not in acceptor_source_text
    require(acceptor_source_text, "IocpOperationKind::Accept", acceptor_source)
    require(acceptor_source_text, "retainCompletionOperation", acceptor_source)
    require(acceptor_source_text, "trackCompletionOperation", acceptor_source)
    require(acceptor_source_text, "operation.completionConsumer", acceptor_source)
    require(acceptor_source_text, "completionPending", acceptor_source)
    require(acceptor_source_text, "CancelIoEx", acceptor_source)
    require(acceptor_source_text, "std::vector<IocpAcceptSlot>", acceptor_source)
    require(acceptor_source_text, "slot.generation", acceptor_source)
    require(acceptor_source_text, "beginAcceptRetry", acceptor_source)
    require(acceptor_source_text, "cancelPendingAccepts(false)", acceptor_source)

    connector_header_text = connector_header.read_text(encoding="utf-8")
    require(connector_header_text, "IocpConnectState", connector_header)
    require(connector_header_text, "std::shared_ptr<IocpConnectState>", connector_header)

    connector_source_text = connector_source.read_text(encoding="utf-8")
    require(connector_source_text, "platform::loadConnectEx", connector_source)
    require(connector_source_text, "platform::bindUnspecified", connector_source)
    require(connector_source_text, "platform::updateConnectContext", connector_source)
    assert "platform::bindUnspecifiedOrDie" not in connector_source_text
    assert "platform::updateConnectContextOrDie" not in connector_source_text
    require(connector_source_text, "IocpOperationKind::Connect", connector_source)
    assert "preserveSocketAssociation" not in connector_source_text
    require(connector_source_text, "retainCompletionOperation", connector_source)
    require(connector_source_text, "trackCompletionOperation", connector_source)
    require(connector_source_text, "operation.completionConsumer", connector_source)
    require(connector_source_text, "completionPending", connector_source)
    require(connector_source_text, "retryAfterCancel", connector_source)
    require(connector_source_text, "ERROR_NOT_FOUND", connector_source)

    accept_connect_drain_text = accept_connect_drain_test.read_text(encoding="utf-8")
    require(
        accept_connect_drain_text,
        "constexpr std::size_t kAcceptDepth = 7",
        accept_connect_drain_test,
    )
    require(
        accept_connect_drain_text,
        "outstandingCompletionCount(loop) == 1",
        accept_connect_drain_test,
    )
    require(
        accept_connect_drain_text,
        "retainedCompletionCount(loop) == 0",
        accept_connect_drain_test,
    )

    tcp_connection_header_text = tcp_connection_header.read_text(encoding="utf-8")
    require(tcp_connection_header_text, "IocpTcpTransport", tcp_connection_header)

    tcp_connection_source_text = tcp_connection_source.read_text(encoding="utf-8")
    require(tcp_connection_source_text, "iocpTransport_->startRead", tcp_connection_source)
    require(tcp_connection_source_text, "iocpTransport_->startWrite", tcp_connection_source)
    require(tcp_connection_source_text, "iocpTransport_->completeRead", tcp_connection_source)
    require(tcp_connection_source_text, "iocpTransport_->completeWrite", tcp_connection_source)

    tcp_client_source_text = tcp_client_source.read_text(encoding="utf-8")
    require(
        tcp_client_source_text,
        "preserveSocketAssociation",
        tcp_client_source)

    tcp_transport_text = tcp_transport.read_text(encoding="utf-8")
    require(tcp_transport_text, "class IocpTcpTransport", tcp_transport)
    require(tcp_transport_text, "[[nodiscard]] int startRead", tcp_transport)
    require(tcp_transport_text, "completeRead", tcp_transport)
    require(tcp_transport_text, "kReadChunkBytes = 4 * 1024", tcp_transport)
    require(tcp_transport_text, "std::unique_ptr<char[]>& readStorage_", tcp_transport)
    require(tcp_transport_text, "std::shared_ptr<SharedState> sharedState_", tcp_transport)
    require(tcp_transport_text, "releaseReadStorage", tcp_transport)
    assert "std::array<char, 65536> readStorage_" not in tcp_transport_text, (
        "idle IOCP connections must not embed the historical 64 KiB read array"
    )
    require(tcp_transport_text, "[[nodiscard]] int startWrite", tcp_transport)
    require(tcp_transport_text, "completeWrite", tcp_transport)
    require(tcp_transport_text, "std::deque<WriteSegment>&", tcp_transport)
    require(tcp_transport_text, "bufferedWriteBytes_", tcp_transport)
    assert "writeStorage_" not in tcp_transport_text, (
        "IOCP writes must not retain a full transport mirror"
    )

    tcp_transport_source_text = tcp_transport_source.read_text(encoding="utf-8")
    require(tcp_transport_source_text, "WSARecv", tcp_transport_source)
    require(
        tcp_transport_source_text,
        "std::make_unique_for_overwrite<char[]>",
        tcp_transport_source,
    )
    require(
        tcp_transport_source_text,
        "IOCP read storage released before completion",
        tcp_transport_source,
    )
    require(tcp_transport_source_text, "WSASend", tcp_transport_source)
    require(tcp_transport_source_text, "IocpOperationKind::Read", tcp_transport_source)
    require(tcp_transport_source_text, "IocpOperationKind::Write", tcp_transport_source)
    require(tcp_transport_source_text, "front.offset += completed", tcp_transport_source)
    require(tcp_transport_source_text, "writeSegments_.pop_front()", tcp_transport_source)
    require(
        tcp_transport_source_text,
        "std::numeric_limits<ULONG>::max",
        tcp_transport_source,
    )
    require(
        tcp_transport_source_text,
        "setIocpWriteChunkLimitForTesting",
        tcp_transport_source,
    )
    require(tcp_transport_source_text, "return error;", tcp_transport_source)
    assert "setRevents" not in tcp_transport_source_text, (
        "a synchronous IOCP submission error must not fabricate Channel readiness"
    )

    sync_error_test_text = sync_error_test.read_text(encoding="utf-8")
    require(sync_error_test_text, "WSAENOBUFS", sync_error_test)
    require(sync_error_test_text, "WSAECONNRESET", sync_error_test)
    require(sync_error_test_text, "ERROR_OPERATION_ABORTED", sync_error_test)

    segmented_write_test_text = segmented_write_test.read_text(
        encoding="utf-8"
    )
    require(
        segmented_write_test_text,
        "iocpPeakWriteSegmentCountForTesting() ==",
        segmented_write_test,
    )
    require(
        segmented_write_test_text,
        "iocpCurrentBufferedWriteBytesForTesting() == 0",
        segmented_write_test,
    )
    partial_write_test_text = partial_write_test.read_text(encoding="utf-8")
    require(
        partial_write_test_text,
        "iocpPartialWriteCompletionCountForTesting() > 1",
        partial_write_test,
    )
    read_storage_test_text = read_storage_test.read_text(encoding="utf-8")
    require(
        read_storage_test_text,
        "iocpCurrentReadStorageBytesForTesting() == 0",
        read_storage_test,
    )
    require(
        read_storage_test_text,
        "iocpReadStorageAllocationCountForTesting() == 1",
        read_storage_test,
    )
    require(
        read_storage_test_text,
        "observedReadCancellation.load(",
        read_storage_test,
    )
    require(
        partial_write_test_text,
        "iocpMaxWriteSubmissionBytesForTesting() <=",
        partial_write_test,
    )

    poller_header_text = poller_header.read_text(encoding="utf-8")
    require(poller_header_text, "Windows IOCP backend for EventLoop", poller_header)
    require(poller_header_text, "fixed typed terminal notices", poller_header)
    assert "skeleton" not in poller_header_text.lower(), (
        "IocpPoller comments must describe the active data path, not the old skeleton milestone"
    )

    poller_text = poller_source.read_text(encoding="utf-8")
    require(poller_text, "GetQueuedCompletionStatusEx", poller_source)
    require(
        poller_header_text,
        "std::atomic<bool> wakeupPending_{false}",
        poller_header,
    )
    require(
        poller_text,
        "wakeupPending_.compare_exchange_strong",
        poller_source,
    )
    require(
        poller_text,
        "wakeupPending_.store(false, std::memory_order_release)",
        poller_source,
    )
    wakeup_coalescing_test_text = wakeup_coalescing_test.read_text(encoding="utf-8")
    require(
        wakeup_coalescing_test_text,
        "logicalWakeupCount(loop) ==",
        wakeup_coalescing_test,
    )
    require(
        wakeup_coalescing_test_text,
        "physicalWakeupPacketsPosted() ==",
        wakeup_coalescing_test,
    )
    require(poller_text, "waitNativeCompletionNotices", poller_source)
    require(poller_text, "CompletionNotice", poller_source)
    require(poller_text, "retireIocpOperationSubmission", poller_source)
    require(poller_text, "operation->terminalObserver", poller_source)
    require(poller_text, "reinterpret_cast<IocpOperation*>", poller_source)
    require(poller_text, "operation->bytesTransferred", poller_source)
    assert "setIocpCompletionOperation(operation)" not in poller_text, (
        "production IOCP publication must not restore the Channel operation mailbox"
    )
    require(poller_text, "takeNextDirectCompletionNotice", poller_source)
    require(poller_text, "pendingDirectCompletionNoticeCount", poller_source)
    for retired_fragment in (
        "publishCompletionNotices",
        "publishedChannels",
        "completionEvents",
        "appendIocpAcceptCompletionOperation",
    ):
        assert retired_fragment not in poller_text, (
            f"legacy fake-readiness publication remains in {poller_source}: "
            f"{retired_fragment}"
        )
    require(
        poller_text,
        "IocpPoller::poll compatibility shell is retired",
        poller_source,
    )
    require(poller_text, "IocpOperationKind::Read", poller_source)
    require(poller_text, "IocpOperationKind::Write", poller_source)
    require(poller_text, "associatedFds_", poller_source)

    poller_header_text = poller_header.read_text(encoding="utf-8")
    require(poller_header_text, "kCompletionBatchSize = 64", poller_header)
    require(poller_header_text, "completionBatchSize_", poller_header)
    require(poller_header_text, "IocpCompletionState", poller_header)
    assert "deferredEntries_" not in poller_header_text

    poller_contract_text = poller_contract_test.read_text(encoding="utf-8")
    require(poller_contract_text, "testBoundedIocpBatch", poller_contract_test)
    require(
        poller_contract_text,
        "testSameChannelCompletionsStayDistinctWithoutFakeReadiness",
        poller_contract_test,
    )
    require(
        poller_contract_text,
        "testDirectTerminalErrorSurvivesObserverRemoval",
        poller_contract_test,
    )
    require(
        poller_contract_text,
        "configuredCompletionBatchSize(loop)",
        poller_contract_test,
    )
    require(
        poller_contract_text,
        "testIocpCompletionBudgetMetrics",
        poller_contract_test,
    )

    completion_engine_text = completion_engine_test.read_text(encoding="utf-8")
    require(completion_engine_text, "testNativePacketsBecomeDistinctTerminalNotices", completion_engine_test)
    require(completion_engine_text, "testGenerationRejectsDuplicateAndRejectedSubmissionPackets", completion_engine_test)
    require(completion_engine_text, "testObserverRevokeDoesNotRetireKernelLeaseEarly", completion_engine_test)
    require(completion_engine_text, "testEventLoopDirectlyDispatchesAllOperationKindsWithinOwnerBudget", completion_engine_test)
    require(completion_engine_text, "testDirectDispatchRevalidatesObserverAfterReentry", completion_engine_test)
    require(completion_engine_text, "testSubmissionCapturesObserverBeforeNativeDequeue", completion_engine_test)
    require(completion_engine_text, "testSameAddressObserverReplacementCannotReviveOldCompletion", completion_engine_test)
    require(completion_engine_text, "terminalCalls == 4", completion_engine_test)
    require(completion_engine_text, "terminalCalls == 2", completion_engine_test)

    event_loop_text = event_loop_source.read_text(encoding="utf-8")
    require(event_loop_text, "takeNextCompletionNotice", event_loop_source)
    require(event_loop_text, "completionObserverCurrent", event_loop_source)
    require(event_loop_text, "notice.consumer(", event_loop_source)
    adapter_text = io_engine_adapter.read_text(encoding="utf-8")
    require(adapter_text, "waitCompletionEngine", io_engine_adapter)
    assert "return NativePoller::poll(timeoutMs, notices.readiness_)" not in adapter_text
    access_text = iocp_poller_access.read_text(encoding="utf-8")
    require(access_text, "return poller.waitNativeCompletionNotices(timeoutMs)", iocp_poller_access)
    assert "publishCompletionNotices" not in access_text, (
        "production IOCP Engine access must not publish any Completion kind through Channel"
    )
    channel_source_text = channel_source.read_text(encoding="utf-8")
    for retired_fragment in (
        "setIocpCompletionOperation",
        "takeIocpCompletionOperation",
        "appendIocpAcceptCompletionOperation",
        "takeIocpAcceptCompletionOperation",
        "clearIocpAcceptCompletionOperations",
        "iocpCompletionOperation_",
        "iocpAcceptCompletionHead_",
        "iocpAcceptCompletionTail_",
    ):
        assert retired_fragment not in channel_source_text, (
            f"Channel still implements IOCP operation storage in {channel_source}: "
            f"{retired_fragment}"
        )
    channel_header_text = channel_header.read_text(encoding="utf-8")
    require(channel_header_text, "IocpOperation* iocpCompletionOperation_", channel_header)
    require(channel_header_text, "IocpOperation* iocpAcceptCompletionHead_", channel_header)
    require(channel_header_text, "IocpOperation* iocpAcceptCompletionTail_", channel_header)
    harness_text = iocp_test_harness.read_text(encoding="utf-8")
    require(harness_text, "ioEngineFromPoller", iocp_test_harness)
    assert "poller_->poll" not in harness_text, (
        "repository contracts must exercise the production Engine path, not legacy Poller::poll"
    )
    assert "takeIocpAcceptCompletionOperation" not in acceptor_source_text
    assert "clearIocpAcceptCompletionOperations" not in acceptor_source_text
    require(
        event_loop_text,
        "lifecycleState_->phase != EventLoopPhase::Running",
        event_loop_source,
    )
    lifecycle_hub_text = lifecycle_hub_test.read_text(encoding="utf-8")
    require(
        lifecycle_hub_text,
        "testShutdownPhaseSequenceIsObservableAndMonotonic",
        lifecycle_hub_test,
    )
    require(
        lifecycle_hub_text,
        "EventLoopPhase::FinalDraining",
        lifecycle_hub_test,
    )
    quit_completion_drain_text = quit_completion_drain_test.read_text(
        encoding="utf-8"
    )
    require(
        quit_completion_drain_text,
        "observedReadCompletionPhase",
        quit_completion_drain_test,
    )
    require(
        quit_completion_drain_text,
        "EventLoopPhase::Quiescing",
        quit_completion_drain_test,
    )

    transport_text = tcp_transport_source.read_text(encoding="utf-8")
    require(transport_text, "readCompletionPending", tcp_transport_source)
    require(transport_text, "writeCompletionPending", tcp_transport_source)
    require(transport_text, "sharedState_", tcp_transport_source)

    core_cmake_text = core_cmake.read_text(encoding="utf-8")
    require(core_cmake_text, "net/platform/IocpSocketOps_win.cc", core_cmake)
    require(core_cmake_text, "net/platform/IocpTcpTransport_win.cc", core_cmake)
    require(core_cmake_text, "if(GAMENET_BUILD_TESTING)", core_cmake)
    require(
        core_cmake_text,
        "GAMENET_INTERNAL_IOCP_TEST_HOOKS=1",
        core_cmake,
    )
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    require(
        tests_cmake_text,
        "test_tcp_connection_iocp_segmented_write.cpp threading lifecycle",
        tests_cmake,
    )
    require(
        tests_cmake_text,
        "test_tcp_connection_iocp_partial_write.cpp threading lifecycle",
        tests_cmake,
    )
    require(
        tests_cmake_text,
        "test_tcp_connection_iocp_read_storage.cpp threading lifecycle",
        tests_cmake,
    )
    require(
        tests_cmake_text,
        "test_acceptor_iocp_pool.cpp threading lifecycle",
        tests_cmake,
    )

    acceptor_pool_test_text = acceptor_pool_test.read_text(encoding="utf-8")
    require(
        acceptor_pool_test_text,
        "testFixedPoolBurstAndStopReentry",
        acceptor_pool_test,
    )
    require(
        acceptor_pool_test_text,
        "testSynchronousFailureCancelsGenerationBeforeRetry",
        acceptor_pool_test,
    )
    require(
        acceptor_pool_test_text,
        "currentSubmitted == kDepth",
        acceptor_pool_test,
    )
    require(
        acceptor_pool_test_text,
        "submissions == observations.completions",
        acceptor_pool_test,
    )

    guard_command = "python3 tests/cmake/test_windows_iocp_data_path_contract.py"
    require(ci_docs.read_text(encoding="utf-8"), "test_windows_iocp_data_path_contract.py", ci_docs)
    require(workflow.read_text(encoding="utf-8"), guard_command, workflow)
    require(ci_contract.read_text(encoding="utf-8"), guard_command, ci_contract)


if __name__ == "__main__":
    main()
