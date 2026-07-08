# Migration Status

Last checked: 2026-07-08

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
| 3 | Split CMake targets and test structure | Present: `gamenet_core`, `GameNet::core`, install/export package config, echo examples, unit/contract/integration test directories, scope/intent/documentation guards, install consumer fixture, and Acceptor/Buffer/Channel/Connector/InetAddress/Poller/Socket/TcpClient/TcpServer/TcpConnection/EventLoopThread/EventLoopThreadPool contract tests |
| 4 | Gradually migrate protocol / transport / game foundation / experimental | Deferred: intent files are preserved as design assets only |

## Verification State

The current worktree configures 54 configured CTest tests for the Reactor / TCP
foundation: 6 unit tests, 47 contract tests, and 1 integration test.

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON`
- Build: `cmake --build build --parallel`
- Test: `ctest --test-dir build --output-on-failure`
- Current result: Latest remote CI run: ci #17 for commit 4cdf589 on main
  completed successfully on 2026-07-07. The successful workflow included
  `Linux CMake build and tests`, `Linux ASan/UBSan build and tests`,
  `Linux Release build`, and `Windows MSVC IOCP build and tests`.
- Race-oriented CI: this worktree adds a `linux-tsan` workflow job named
  `Linux TSan race-oriented build and tests`. It configures with
  `GAMENET_ENABLE_TSAN=ON`, builds the Debug target set, and runs the CTest
  suite filtered to the `threading` label. That label includes cross-thread
  APIs, worker-loop scheduling, pending read/write forceClose cancel-close
  contracts, mixed-timing pending ConnectEx stop contracts,
  active retry-enabled TcpClient stop-after-peer-close contracts,
  active cross-thread TcpClient disconnect contracts,
  active cross-thread TcpClient connect contracts, mixed-timing pending-read forceClose contracts,
  mixed-timing pending-write forceClose contracts,
  worker-owned active-write stop contracts, and worker-callback TcpServer stop
  contracts. Remote green evidence for this new job is pending until the next workflow run.
- Scope guard: local self-test and repository scan pass; CI runs both before
  CMake configure.
- Intent/documentation guards: CI runs the intent consistency guard, TCP lifecycle contract guard, TcpConnection context contract guard, EventLoopThreadPool contract guard, TimerQueue contract guard, threading gate contract guard, migration status contract guard, install/package contract guard, MSVC UTF-8 build contract guard, platform backend contract guard, Windows IOCP milestone contract guard, Windows IOCP data-path contract guard, sanitizer flag contract guard, Release-safe test guard, and workflow job structure guard before CMake configure.
- Added lifecycle coverage in this worktree: server stop with active connections, server stop during active write, server stop soak for worker-owned connections, server multi-worker stop from the base loop, server worker-owned active-write stop, server worker-callback TcpServer stop soak, client retry stop race, client retry-stop soak, client stop during pending ConnectEx, client pending ConnectEx stop soak, client cross-thread stop during pending ConnectEx, client mixed-timing pending ConnectEx stop soak, client destruction during pending ConnectEx, client destruction with active TcpConnection, client mixed-timing active-connection stop soak, client cross-thread active disconnect, client cross-thread active connect, peer close convergence, peer reset convergence, error-triggered teardown idempotence, cross-thread send delivery, write-complete callback ordering, shutdown while output pending, cross-thread shutdown draining, high-water mark notification, repeated forceClose idempotence, cross-thread forceClose soak, cross-thread pending-read forceClose, cross-thread pending-write forceClose, pending-read forceClose cancellation before connection destruction, mixed-timing pending-read forceClose soak, pending-write forceClose soak before connection destruction, mixed-timing pending-write forceClose soak, TimerQueue ready-timer cancellation race coverage, EventLoopThreadPool queued-work soak coverage, and EventLoopThreadPool restart-stop soak coverage.
- Sanitizers: CI includes an ASan/UBSan Debug build and CTest job for the
  Reactor / TCP foundation. The worktree also defines a Linux TSan
  race-oriented threading test gate for thread-affinity and lifecycle risks.
- Release: CI includes a Release build and CTest gate for the Reactor / TCP
  foundation. C++ tests use a Release-safe helper instead of standard `assert`
  so contract checks remain active with `NDEBUG`.
- Install/package: CI installs the core target and builds an external consumer
  fixture through `find_package(GameNetCore)` and `GameNet::core`.
- Windows: Windows support is now represented by a `windows-msvc` workflow job
  for the Reactor / TCP IOCP backend. Local VS2026 Debug configure/build
  succeeds after the MSVC `/utf-8` and `/FS` compile options were added, and a
  local full Windows CTest run with a 10-second per-test timeout passes 54/54
  configured tests with 0 failing tests. The IOCP data path now covers
  `contract.event_loop.test_event_loop`,
  `contract.event_loop_thread_pool.test_event_loop_thread_pool`,
  `contract.timer_queue.test_timer_queue`,
  `contract.acceptor.test_acceptor_contract`,
  `contract.connector.test_connector_contract`,
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
  `contract.tcp_client.test_tcp_client_cross_thread_connect`,
  `contract.tcp_server.test_tcp_server_contract`,
  `contract.tcp_server.test_tcp_server_stop_active_connections`,
  `contract.tcp_server.test_tcp_server_stop_active_write`,
  `contract.tcp_server.test_tcp_server_stop_multi_worker`,
  `contract.tcp_server.test_tcp_server_stop_worker_active_write_soak`,
  `contract.tcp_server.test_tcp_server_stop_from_worker_callback_soak`,
  `contract.tcp_server.test_tcp_server_stop_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_send`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_write`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_shutdown`,
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
  consumer gates; remote green status is established by ci #17 on main.
  The IOCP data-path design and implementation plan are recorded in
  `docs/superpowers/specs/2026-07-07-windows-iocp-data-path-design.md` and
  `docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`. See
  `docs/development/windows_iocp_milestone.md`.

Before promoting Phase 4 work, keep the Linux and Windows CI gates green and
add any missing local, soak, race-oriented, or platform-specific verification as
its own focused migration step.
