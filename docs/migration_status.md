# Migration Status

Last checked: 2026-07-10

## Current Task Goal

`game-net-core` is the component-split migration target for the larger
`mini_trantor` project.

The active execution target is still the Reactor / TCP foundation. Higher-level
protocol, transport, game foundation, and experimental modules remain deferred
until the current core has stable targets, tests, and examples.

## Phase Status

| Phase | Scope | Current status |
| --- | --- | --- |
| 1 | Initialize the `game-net-core` project skeleton | Present: top-level CMake, README, AGENTS, docs, intents, rules, include/src/tests/examples layout |
| 2 | Migrate Reactor / TCP core | Present: base utilities, socket helpers, Channel/Poller/EventLoop/TimerQueue, Acceptor/Connector, TcpConnection/TcpServer/TcpClient |
| 3 | Split CMake targets and test structure | Present: `gamenet_core`, `GameNet::core`, install/export package config, echo examples, unit/contract/integration test directories, scope/intent/documentation guards, install consumer fixture, an opt-in core benchmark target, and Acceptor/Buffer/Channel/Connector/InetAddress/Poller/Socket/TcpClient/TcpServer/TcpConnection/EventLoopThread/EventLoopThreadPool contract tests |
| 4 | Gradually migrate protocol / transport / game foundation / experimental | Deferred: intent files are preserved as design assets only |

## Verification State

The current worktree configures 67 configured CTest tests for the Reactor / TCP
foundation: 7 unit tests, 59 contract tests, and 1 integration test.

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON`
- Build: `cmake --build build --parallel`
- Test: `ctest --test-dir build --output-on-failure`
- Last fully validated commit: `9b27a0a3c3993cb1f90ef4357fa80027205ca221`.
- CI workflow run id: `28948507704` (`ci` #23).
- Validation date: 2026-07-08.
- Result: all five jobs passed:
  `Linux CMake build and tests`, `Linux ASan/UBSan build and tests`,
  `Linux TSan race-oriented build and tests`, `Linux Release build`, and
  `Windows MSVC IOCP build and tests`.
- Most recent audited candidate: commit
  `0d61658ad19e8758dbf8119a3444a587e7a54a5a`, `ci` run id
  `29059799283` (#25), completed 2026-07-10. Windows MSVC IOCP passed;
  the four Linux jobs failed on the same
  `contract.tcp_client.test_tcp_client_repeated_connect` assertion because a
  completed Connector attempt still occupied `channel_` until a queued reset.
  The current worktree releases Connector member ownership immediately while
  deferring only actual Channel destruction; fresh remote validation is still required.
- Race-oriented CI: this worktree adds a `linux-tsan` workflow job named
  `Linux TSan race-oriented build and tests`. It configures with
  `GAMENET_ENABLE_TSAN=ON`, builds the Debug target set, and runs the CTest
  suite filtered to the `threading` label. That label includes cross-thread
  APIs, worker-loop scheduling, pending read/write forceClose cancel-close
  contracts, direct Connector retry-stop cancellation contracts,
  mixed-timing pending ConnectEx stop contracts,
  active retry-enabled TcpClient stop-after-peer-close contracts,
  active cross-thread TcpClient disconnect contracts,
  repeated active TcpClient disconnect idempotence contracts,
  repeated active TcpClient stop idempotence contracts,
  repeated active TcpClient connect idempotence contracts,
  active cross-thread TcpClient connect contracts,
  active cross-thread TcpClient retry configuration contracts,
  post-close TcpConnection send ignore contracts, mixed-timing pending-read forceClose contracts,
  mixed-timing pending-write forceClose contracts,
  repeated TcpConnection shutdown idempotence contracts,
  worker-owned active-write stop contracts, worker-callback TcpServer stop
  contracts, and repeated TcpServer stop idempotence contracts.
  Latest recorded race-oriented CI remote green evidence is ci #23 on main.
- Scope guard: local self-test and repository scan pass; CI runs both before
  CMake configure.
- Intent/documentation guards: CI runs the intent consistency guard, intent metadata contract guard, Core benchmark contract guard, Logger thread-contract guard, EventLoop contract guard, TCP lifecycle contract guard, TcpConnection context contract guard, TcpConnection thread-contract guard, EventLoopThreadPool contract guard, TimerQueue contract guard, threading gate contract guard, migration status contract guard, install/package contract guard, MSVC UTF-8 build contract guard, platform backend contract guard, Windows IOCP milestone contract guard, Windows IOCP data-path contract guard, sanitizer flag contract guard, Release-safe test guard, and workflow job structure guard before CMake configure. The EventLoop contract guard now also requires the cross-thread-observed pending functor execution state to be atomic or synchronized.
- Intent governance: all 52 formal `*.intent.md` documents now carry ordered
  `status`, `target`, `migration_source`, and `promote_gate` front matter and
  appear exactly once in the intent index: 18 active `GameNet::core` contracts,
  23 deferred design assets, and 11 legacy source-project stage documents.
  Active bodies reject stale `MINI_ENABLE_*`, `mini::`, `mini/net`, and
  `mini-trantor` contracts. Deferred bodies require an explicit future gate;
  legacy v1/v2/v3/v5/v6 and M1-M32 documents target `historical` with
  `promote_gate: never`, so their old options, test counts, and phase claims are
  not current repository evidence. The implemented echo use case is migrated
  to `GameNet::core` and names its integration contract.
- Added lifecycle and base coverage in this worktree: coost-compatible Logger unit and contract coverage, concurrent Logger runtime-configuration coverage (`contract.base.test_logger_thread_safety`), EventLoop cross-thread pending-functor execution-state atomicity guard, cross-thread TcpConnection state observation (`contract.tcp_connection.test_tcp_connection_cross_thread_state`), Connector completed-Channel member release guarded by the repeated-connect contract, server stop with active connections, server stop during active write, server stop soak for worker-owned connections, server multi-worker stop from the base loop, server worker-owned active-write stop, server worker-callback TcpServer stop soak, server repeated stop idempotence (`contract.tcp_server.test_tcp_server_repeated_stop`), client retry stop race, client retry-stop soak, direct Connector retry-stop cancellation (`contract.connector.test_connector_retry_stop`), client stop during pending ConnectEx, client pending ConnectEx stop soak, client cross-thread stop during pending ConnectEx, client mixed-timing pending ConnectEx stop soak, client destruction during pending ConnectEx, client destruction with active TcpConnection, client mixed-timing active-connection stop soak, client cross-thread active disconnect, client repeated active disconnect idempotence, client repeated active stop idempotence (`contract.tcp_client.test_tcp_client_repeated_stop`), client repeated active connect idempotence (`contract.tcp_client.test_tcp_client_repeated_connect`), client cross-thread active connect, client cross-thread retry configuration (`contract.tcp_client.test_tcp_client_cross_thread_retry_config`), peer close convergence, peer reset convergence, error-triggered teardown idempotence, cross-thread send delivery, post-close TcpConnection send ignore (`contract.tcp_connection.test_tcp_connection_send_after_close`), write-complete callback ordering, shutdown while output pending, cross-thread shutdown draining, repeated TcpConnection shutdown idempotence (`contract.tcp_connection.test_tcp_connection_repeated_shutdown`), high-water mark notification, repeated forceClose idempotence, repeated connectDestroyed stale-registration cleanup (`contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed`), cross-thread forceClose soak, cross-thread pending-read forceClose, cross-thread pending-write forceClose, pending-read forceClose cancellation before connection destruction, mixed-timing pending-read forceClose soak, pending-write forceClose soak before connection destruction, mixed-timing pending-write forceClose soak, TimerQueue ready-timer cancellation race coverage, EventLoopThreadPool queued-work soak coverage, and EventLoopThreadPool restart-stop soak coverage.
- Test support hardening: repeated TcpConnection lifecycle/race setup now uses
  shared tests/support helpers. `SocketPair.h` centralizes socketpair,
  nonblocking, and small-send-buffer setup; `ClientSocket.h` centralizes nonblocking test-client connect and cleanup
  for TcpServer lifecycle contracts, Acceptor/Socket contracts, and
  TcpConnection peer-reset setup; `TcpConnectionHarness.h`
  centralizes loop-bound TcpConnection construction from connected socketpair
  fixtures; `TcpConnectionCallbacks.h`
  centralizes owner-loop connection/disconnection/close callback counting for
  force-close contracts; `LoopTest.h` centralizes EventLoop watchdog execution
  for TcpConnection force-close contracts, TcpClient lifecycle watchdogs, and
  TcpServer lifecycle watchdogs;
  `ThreadHandoff.h` centralizes one-shot and delayed non-owner-thread handoff
  for cross-thread lifecycle contracts; `FutureTest.h` centralizes bounded future waits for EventLoop, EventLoopThread, TimerQueue, and EventLoopThreadPool async contract tests; and
  `TcpClientStopHarness.h` centralizes retry-stop stale-reconnect assertions
  for client lifecycle contracts; `TcpServerHarness.h` centralizes multi-client TcpServer connection setup and worker-loop distribution assertions
  for server lifecycle/race contracts.
- Sanitizers: CI includes an ASan/UBSan Debug build and CTest job for the
  Reactor / TCP foundation. The worktree also defines a Linux TSan
  race-oriented threading test gate for thread-affinity and lifecycle risks.
- Long soak: this worktree adds a non-default `long-soak` workflow. It is
  manual-only through `workflow_dispatch` and runs the `threading` CTest slice
  with `ctest --repeat until-fail` so mixed-timing lifecycle contracts can
  gather stronger soak evidence without blocking ordinary push or pull-request
  CI. The long-soak repository guard parity includes the EventLoop contract guard,
  keeping manual soak guards aligned with the ordinary CI guard surface. Local Windows Debug long-soak evidence currently covers the previous 43-test threading slice
  before the cross-thread TcpClient retry configuration contract expanded the threading label to 44 tests. The workflow default repeat shape was:
  `ctest --test-dir build -C Debug --output-on-failure -L threading --repeat until-fail:20 --timeout 60`;
  43/43 threading-labeled tests passed across 20 repeats on 2026-07-09,
  and CTest reported total test time was 637.56 seconds. The then-expanded
  44-test threading slice was covered once by the full Windows Debug and Release
  CTest checkpoint. The new cross-thread TcpConnection state contract expands the
  threading slice to 45 tests. The Logger concurrency contract expands the current
  threading slice to 46 tests; remote repeat-soak evidence for that expanded
  slice is not recorded here.
  Local Windows Debug preflight for this worktree passes all 46 threading tests
  across 5 repeats with `--timeout 15`; CTest reported 163.10 seconds on
  2026-07-10. This is a local preflight, not the required remote repeat-50 evidence.
  Remote GitHub `long-soak` evidence is now recorded:
  run 28986707243, job 86017363504, commit 9b27a0a3c3993cb1f90ef4357fa80027205ca221,
  repeat 20, timeout 60 seconds, completed successfully at
  2026-07-09T01:15:38Z with 36/36 threading-labeled tests passed in
  608.67 seconds.
- Release: CI includes a Release build and CTest gate for the Reactor / TCP
  foundation. C++ tests use a Release-safe helper instead of standard `assert`
  so contract checks remain active with `NDEBUG`. Local Windows Release
  evidence now also passes after shortening generated CMake test target names
  to keep MSBuild `.tlog` paths below Windows path-length limits:
  `cmake --build build-release --config Release --parallel` succeeds, and
  `ctest --test-dir build-release -C Release --output-on-failure --timeout 10`
  reports 67/67 Release tests passed for the current worktree in 39.85 seconds
  on 2026-07-10.
- Core benchmark: `GAMENET_BUILD_BENCHMARKS` defaults to `OFF`; when enabled it
  builds the non-installed, non-CTest `gamenet_core_benchmark` executable. Its
  `gamenet.core_benchmark.v1` JSON covers echo throughput/P50/P99, one-versus-two
  worker scaling, idle-connection process working set, and slow-client output
  accumulation while identifying backend and completion mode. Local Windows
  MSVC Release evidence on 2026-07-10 records 51.979 MiB/s and P99 61.7 us with
  one worker, 73.994 MiB/s and P99 59.2 us with two workers, about 71,264 bytes
  of process working-set delta per idle connection at 256 connections, and a
  67,465,216-byte working-set increase after offering 8 MiB to each of four
  non-reading clients. The slow-client run fired four high-water callbacks and
  explicitly reports `high_water_notification_only`; it is not a memory-cap
  claim. Raw local evidence is under
  `docs/development/benchmark_results/2026-07-10-windows-msvc-release/`.
  The manual-only `core-benchmark` workflow is ready to produce matching Linux
  epoll and Windows IOCP Release artifacts from one SHA.
  Linux Release JSON remains required before PR-0C has cross-platform evidence.
- Install/package: CI installs the core target and builds an external consumer
  fixture through `find_package(GameNetCore)` and `GameNet::core`. Release
  install/package consumer also passes locally: the Release build installs to
  `build-release/_install`, the external fixture configures into
  `build-release-install-consumer`, builds in Release, and the
  `gamenet_install_consumer` executable exits 0.
- Windows: Windows support is now represented by a `windows-msvc` workflow job
  for the Reactor / TCP IOCP backend. Local VS2026 Debug configure/build
  succeeds after the MSVC `/utf-8` and `/FS` compile options were added, and a
  local full Windows Debug CTest run with a 10-second per-test timeout passes
  67/67 configured tests with 0 failing tests in 48.41 seconds on 2026-07-10.
  The IOCP data path now covers
  `contract.event_loop.test_event_loop`,
  `unit.base.test_logger`,
  `contract.base.test_logger_contract`,
  `contract.base.test_logger_thread_safety`,
  `contract.event_loop_thread_pool.test_event_loop_thread_pool`,
  `contract.timer_queue.test_timer_queue`,
  `contract.acceptor.test_acceptor_contract`,
  `contract.connector.test_connector_contract`,
  `contract.connector.test_connector_retry_stop`,
  `contract.poller.test_poller_contract`,
  `contract.tcp_client.test_tcp_client_contract`,
  `contract.tcp_client.test_tcp_client_retry_stop_soak`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect_soak`,
  `contract.tcp_client.test_tcp_client_cross_thread_stop_pending_connect`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect_mixed_timing_soak`,
  `contract.tcp_client.test_tcp_client_destroy_pending_connect`,
  `contract.tcp_client.test_tcp_client_destroy_active_connection`,
  `contract.tcp_client.test_tcp_client_stop_active_connection_mixed_timing_soak`,
  `contract.tcp_client.test_tcp_client_cross_thread_disconnect_active`,
  `contract.tcp_client.test_tcp_client_repeated_disconnect`,
  `contract.tcp_client.test_tcp_client_repeated_stop`,
  `contract.tcp_client.test_tcp_client_repeated_connect`,
  `contract.tcp_client.test_tcp_client_cross_thread_connect`,
  `contract.tcp_client.test_tcp_client_cross_thread_retry_config`,
  `contract.tcp_server.test_tcp_server_contract`,
  `contract.tcp_server.test_tcp_server_stop_active_connections`,
  `contract.tcp_server.test_tcp_server_stop_active_write`,
  `contract.tcp_server.test_tcp_server_stop_multi_worker`,
  `contract.tcp_server.test_tcp_server_stop_worker_active_write_soak`,
  `contract.tcp_server.test_tcp_server_stop_from_worker_callback_soak`,
  `contract.tcp_server.test_tcp_server_repeated_stop`,
  `contract.tcp_server.test_tcp_server_stop_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_send`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_state`,
  `contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed`,
  `contract.tcp_connection.test_tcp_connection_send_after_close`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_write`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_shutdown`,
  `contract.tcp_connection.test_tcp_connection_repeated_shutdown`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_read`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_read_mixed_timing_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_mixed_timing_soak`,
  TcpConnection lifecycle/read/write/cancel-close contracts, and
  `integration.tcp.test_tcp_server_client_echo` through
  `PostQueuedCompletionStatus`, `AcceptEx`, `ConnectEx`, `WSARecv`, and
  `WSASend`. The active Windows source selection points at the IOCP backend,
  the legacy select backend files have been removed from the active target, and
  a select-based Windows CI job must not be promoted as the performance target.
  The Windows install/package consumer gate also passes locally: the VS2026
  Debug build installs to `build-local-vs2026/_install`, and the external
  `tests/cmake/install_consumer` fixture configures, builds, and runs through
  `find_package(GameNetCore)` and `GameNet::core`. The Windows workflow uses
  the Visual Studio generator, Debug CTest, install, and external package
  consumer gates; latest recorded remote green status is ci #23 on main.
  The IOCP data-path design and implementation plan are recorded in
  `docs/superpowers/specs/2026-07-07-windows-iocp-data-path-design.md` and
  `docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`. See
  `docs/development/windows_iocp_milestone.md`.

## Phase 4 Readiness Gate

Phase 4 remains deferred until every gate item below has current evidence:

- Fresh latest-HEAD remote CI evidence is recorded for Linux CMake, Linux ASan/UBSan, Linux TSan, Linux Release, and Windows MSVC IOCP.
- The remote `long-soak` workflow has a green run recorded with run id, commit sha, repeat count, timeout, date, and result.
- `docs/migration_status.md`, `docs/development/ci.md`, and `docs/development/windows_iocp_milestone.md` have no pending evidence.
- TcpConnection, TcpClient/Connector, and TcpServer lifecycle/race tests have no known flaky entries.
- Linux and Windows install/package consumers pass through `find_package(GameNetCore)` and `GameNet::core`.
- Matching Release `gamenet.core_benchmark.v1` evidence is recorded for Linux
  epoll and Windows IOCP with commands, scenario parameters, backend, and
  completion mode; the current worktree has local Windows evidence only.
- The first Phase 4 design-only PR should target protocol framing / PacketFramer.
- HTTP, RPC, and game pipeline modules must stay deferred until protocol framing has its own approved intent, invariants, contracts, and tests.

Before promoting Phase 4 work, keep the Linux and Windows CI gates green and
add any missing local, soak, race-oriented, or platform-specific verification as
its own focused migration step.
