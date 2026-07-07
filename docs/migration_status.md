# Migration Status

Last checked: 2026-07-07

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

The current worktree configures 29 configured CTest tests for the Reactor / TCP
foundation: 6 unit tests, 22 contract tests, and 1 integration test.

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON`
- Build: `cmake --build build --parallel`
- Test: `ctest --test-dir build --output-on-failure`
- Current result: pending fresh Linux CI execution for the newly added lifecycle
  and intent/documentation guards.
- Last remote baseline before these contract additions: 21/21 tests passed.
- Scope guard: local self-test and repository scan pass; CI runs both before
  CMake configure.
- Intent/documentation guards: CI runs the intent consistency guard, TCP lifecycle contract guard, TcpConnection context contract guard, migration status contract guard, install/package contract guard, MSVC UTF-8 build contract guard, platform backend contract guard, Windows IOCP milestone contract guard, Windows IOCP data-path contract guard, sanitizer flag contract guard, Release-safe test guard, and workflow job structure guard before CMake configure.
- Added lifecycle coverage in this worktree: server stop with active connections, client retry stop race, peer close convergence, peer reset convergence, error-triggered teardown idempotence, write-complete callback ordering, shutdown while output pending, high-water mark notification, and repeated forceClose idempotence.
- Sanitizers: CI includes an ASan/UBSan Debug build and CTest job for the
  Reactor / TCP foundation.
- Release: CI includes a Release build and CTest gate for the Reactor / TCP
  foundation. C++ tests use a Release-safe helper instead of standard `assert`
  so contract checks remain active with `NDEBUG`.
- Install/package: CI installs the core target and builds an external consumer
  fixture through `find_package(GameNetCore)` and `GameNet::core`.
- Windows: Windows support is not promoted into CI yet. Local VS2026 Debug
  configure/build succeeds after the MSVC `/utf-8` compile option was added,
  and a local full Windows CTest run with a 10-second per-test timeout passes
  29/29 configured tests with 0 failing tests. The IOCP data path now covers
  `contract.event_loop.test_event_loop`,
  `contract.timer_queue.test_timer_queue`,
  `contract.acceptor.test_acceptor_contract`,
  `contract.connector.test_connector_contract`,
  `contract.poller.test_poller_contract`,
  `contract.tcp_client.test_tcp_client_contract`,
  `contract.tcp_server.test_tcp_server_contract`,
  `contract.tcp_server.test_tcp_server_stop_active_connections`,
  TcpConnection lifecycle/read/write contracts, and
  `integration.tcp.test_tcp_server_client_echo` through
  `PostQueuedCompletionStatus`, `AcceptEx`, `ConnectEx`, `WSARecv`, and
  `WSASend`. The active Windows source selection points at the IOCP backend,
  the legacy select backend files have been removed from the active target, and
  a select-based Windows CI job must not be promoted as the performance target.
  The Windows install/package consumer gate also passes locally: the VS2026
  Debug build installs to `build-local-vs2026/_install`, and the external
  `tests/cmake/install_consumer` fixture configures, builds, and runs through
  `find_package(GameNetCore)` and `GameNet::core`. Windows CI can now be
  proposed as the next platform-specific migration step. The IOCP data-path design and
  implementation plan are recorded in
  `docs/superpowers/specs/2026-07-07-windows-iocp-data-path-design.md` and
  `docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`. See
  `docs/development/windows_iocp_milestone.md`.

Fresh Linux CTest verification for the newly added lifecycle contracts is still
pending remote CI execution.

Before promoting Phase 4 work, keep this CI gate green and add any missing local
or platform-specific verification as its own focused migration step.
