# Windows IOCP Milestone

Last checked: 2026-07-10

Windows support for the Reactor / TCP foundation is represented by the
`windows-msvc` workflow job. The current Windows source selection intentionally
uses `IocpPoller`, and WinSock `select()` is not an accepted fallback for the
active target.

## Current Evidence

Local VS2026 Debug configure and build now succeed with:

```powershell
D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe `
  -S . -B build-local-vs2026 -G "Visual Studio 18 2026" -A x64 `
  -DGAMENET_BUILD_TESTING=ON `
  -DGAMENET_ENABLE_TLS=OFF `
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF

D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe `
  --build build-local-vs2026 --config Debug --parallel
```

Full Windows CTest is now part of the Windows MSVC workflow gate. A local
VS2026 Debug run with a 10-second per-test timeout also passes 67/67 tests with
0 failing tests on 2026-07-10. PR `ci` run `29076601085` (#27) validates
candidate `a7fd77cbd2140041cebb3f900d5c609fafc2adad`; its Windows MSVC IOCP job
`86309502342` passed alongside all four Linux jobs on the same SHA. PR #2 then
merged without rewriting the candidate. Main `ci` run `29079836593` (#29)
passed all five jobs on release commit
`c4818d4b3956c85830e04d4a1f32df4ad701d453`, which is published as annotated
tag `v0.1.0-core-preview`.

The first opt-in Windows MSVC Release performance baseline now records the
current `single_get_queued_completion_status` mode explicitly. Four loopback
JSON artifacts cover one-worker and two-worker echo throughput/P50/P99, 256
idle connections, and four non-reading clients offered 8 MiB each. The raw
evidence lives under
`docs/development/benchmark_results/2026-07-10-windows-msvc-release/`. The
local dirty-worktree baseline is supplemented by manual `core-benchmark` run
`29077151229`, which published the same four `gamenet.core_benchmark.v1`
scenarios as SHA-bound Linux epoll and Windows IOCP Release artifacts. All
eight remote JSON files report `status: ok`. The Windows artifact records
16.188/26.110 MiB/s one/two-worker echo throughput, P99 162.8/151.3 us,
70,848 bytes of working-set delta per connection at 256 connections, and four
high-water callbacks in the slow-client scenario. These are evidence snapshots,
not performance thresholds.

- `contract.event_loop.test_event_loop` and
  `contract.timer_queue.test_timer_queue`: `EventLoop::wakeup()` posts an IOCP
  completion packet through `PostQueuedCompletionStatus`, so cross-thread
  queued work, timer scheduling, and ready-timer cancellation no longer sleep
  until the default poll timeout.
- `contract.event_loop_thread_pool.test_event_loop_thread_pool` and
  `contract.event_loop_thread_pool.test_event_loop_thread_pool_restart_soak`:
  published worker loops accept cross-thread queued work under a light soak,
  execute it on worker loop threads rather than the base loop, and repeated
  start/stop cycles return loop selection to base-loop fallback.
- `unit.base.test_logger` and `contract.base.test_logger_contract`: base
  logging remains synchronous, thread-safe, and independent of EventLoop
  ownership while preserving the configured Windows CTest gate.
- `contract.acceptor.test_acceptor_contract`,
  `contract.tcp_server.test_tcp_server_contract`, and
  `contract.tcp_server.test_tcp_server_stop_active_connections`,
  `contract.tcp_server.test_tcp_server_stop_active_write`,
  `contract.tcp_server.test_tcp_server_stop_multi_worker`,
  `contract.tcp_server.test_tcp_server_stop_worker_active_write_soak`,
  `contract.tcp_server.test_tcp_server_stop_from_worker_callback_soak`,
  `contract.tcp_server.test_tcp_server_repeated_stop`,
  `contract.tcp_server.test_tcp_server_stop_soak`: the Acceptor and server
  stop paths are backed by `AcceptEx` and owner-loop connection teardown;
  worker-loop teardown, including active writes, worker-callback stop()
  reentry, and repeated stop() requests while worker-owned connections are
  active, is allowed to finish before the worker pool is joined.
- `contract.connector.test_connector_contract`,
  `contract.tcp_client.test_tcp_client_contract`, and
  `contract.tcp_client.test_tcp_client_stop_pending_connect`: active connects
  are backed by `ConnectEx`; stop during pending ConnectEx cancels and drains
  the connector-owned completion before a later server start can accept stale
  work, and successful connects preserve the IOCP socket association when the
  fd transfers from Connector to TcpConnection.
- `contract.connector.test_connector_retry_stop`: direct `Connector::stop()`
  after a scheduled retry cancels the retry timer before a later listener can
  receive a stale retry connection.
- `contract.tcp_client.test_tcp_client_retry_stop_soak`: repeated retry-stop
  cycles keep stale retry timers from connecting after a later server start.
- `contract.tcp_client.test_tcp_client_stop_pending_connect_soak`: repeated
  stop cycles during in-flight `ConnectEx` attempts keep stale completions
  from publishing a stopped client connection after a later server start.
- `contract.tcp_client.test_tcp_client_cross_thread_stop_pending_connect`:
  non-owner `stop()` marshals cancellation of an in-flight `ConnectEx` to the
  owner loop before a later server start can publish a stopped connection.
- `contract.tcp_client.test_tcp_client_stop_pending_connect_mixed_timing_soak`:
  immediate and delayed owner/non-owner `stop()` timing during pending
  `ConnectEx` attempts still cancels before a later server start can publish a
  stale connection.
- `contract.tcp_client.test_tcp_client_destroy_pending_connect`: owner-loop
  destruction during a pending `ConnectEx` clears callbacks and cancels
  connector work before a later server start can resurrect a client.
- `contract.tcp_client.test_tcp_client_destroy_active_connection`: owner-loop
  destruction with an active TcpConnection releases client ownership and lets
  peer teardown converge without stale TcpClient callbacks.
- `contract.tcp_client.test_tcp_client_stop_active_connection_mixed_timing_soak`:
  immediate and delayed owner/non-owner `stop()` timing on an active
  retry-enabled connection clears future connect intent before peer close can
  resurrect the client through retry.
- `contract.tcp_client.test_tcp_client_cross_thread_disconnect_active`:
  non-owner `disconnect()` on an active connection marshals graceful teardown to
  the owner loop and converges through the normal close/remove path.
- `contract.tcp_client.test_tcp_client_repeated_disconnect`: repeated owner
  and non-owner `disconnect()` calls on an active connection converge through a
  single client/server teardown path.
- `contract.tcp_client.test_tcp_client_repeated_stop`: repeated owner and
  non-owner `stop()` calls on an active retry-enabled connection prevent peer
  close from resurrecting the client through retry.
- `contract.tcp_client.test_tcp_client_repeated_connect`: repeated owner and
  non-owner `connect()` calls while a client is connecting or active publish at
  most one active client/server connection pair; generation-tagged admission is
  released after terminal no-retry failure so a later explicit connect remains
  available.
- `contract.tcp_client.test_tcp_client_cross_thread_connect`: non-owner
  `connect()` marshals Connector startup to the owner loop and publishes
  connection callbacks on that loop.
- `contract.tcp_client.test_tcp_client_cross_thread_retry_config`: non-owner
  `disableRetry()` marshals retry-state mutation to the owner loop and prevents
  peer close from resurrecting a disabled-retry client.
- `contract.tcp_connection.test_tcp_connection_cross_thread_state`: non-owner
  observers safely read atomic connected/disconnected snapshots across owner-loop
  state transitions.
- `contract.base.test_logger_thread_safety`: concurrent Logger emission and
  runtime callback replacement remain race-free without EventLoop ownership.
- `contract.poller.test_poller_contract`: the Windows Poller contract is backed
  by a posted IOCP read operation, not by a select-style fallback.
- `contract.tcp_connection.test_tcp_connection_lifecycle`,
  `contract.tcp_connection.test_tcp_connection_high_water_mark`,
  `contract.tcp_connection.test_tcp_connection_peer_close`,
  `contract.tcp_connection.test_tcp_connection_peer_reset`,
  `contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_send`,
  `contract.tcp_connection.test_tcp_connection_send_after_close`,
  `contract.tcp_connection.test_tcp_connection_shutdown_pending_output`,
  `contract.tcp_connection.test_tcp_connection_write_complete_ordering`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_write`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_shutdown`,
  `contract.tcp_connection.test_tcp_connection_repeated_shutdown`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_read`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_read`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_read_mixed_timing_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_mixed_timing_soak`, and
  `integration.tcp.test_tcp_server_client_echo`: TcpConnection read/write and
  lifecycle behavior are backed by loop-owned `WSARecv` / `WSASend`
  overlapped operations, including repeated connectDestroyed stale-registration
  cleanup, post-close send suppression, owner-loop and cross-thread force-close
  cancellation of pending read/write operations before connection-owned
  operation storage can be destroyed, and cross-thread/repeated shutdown
  draining pending output before one half-close.

The Windows install/package consumer gate is also part of the Windows MSVC
workflow gate and passes locally. The VS2026 Debug build installs to
`build-local-vs2026/_install`, and the external `tests/cmake/install_consumer`
fixture configures, builds, and runs through `find_package(GameNetCore)` and
`GameNet::core`.

The detailed data-path design is recorded in
`docs/superpowers/specs/2026-07-07-windows-iocp-data-path-design.md`, with the
TDD implementation plan in
`docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`.

## Ownership Model To Finish

- IocpPoller owns the completion port and maps completion keys back to
  loop-owned reactor objects.
- EventLoop owns wakeup state. EventLoop wakeup must complete through IOCP so
  cross-thread `queueInLoop()`, `runAfter()`, and `cancel()` cannot sleep until
  the default poll timeout.
- TcpConnection or a dedicated loop-owned transport helper owns overlapped read/write state.
  No stack `OVERLAPPED` or buffer may outlive the operation that submitted it.
- Channel remains an observer of readiness/completion interest. Poller still
  does not own Channel.
- Socket close, Channel removal, pending overlapped cancellation, and final
  callbacks must have explicit cancel/close ordering.

## CI Gates

The Windows MSVC workflow job must keep these gates green:

- `contract.event_loop.test_event_loop` and
  `contract.timer_queue.test_timer_queue` stay green on Windows without relying
  on the default poll timeout.
- IOCP wakeup has a direct contract that proves cross-thread queued work wakes
  the owner loop.
- EventLoopThreadPool worker publication and queued-work soak stay green on
  Windows through the same EventLoop scheduling API.
- Poller readiness/completion contracts are backed by posted IOCP operations,
  not by a select-style fallback.
- Connector retry-timer cancellation stays green when `stop()` is issued after
  retry scheduling and before a later listener starts.
- TcpConnection read, write, cross-thread send, peer close/reset,
  shutdown-with-pending-output, cross-thread shutdown, repeated shutdown, and
  repeated teardown contracts pass through the IOCP data path.
- Cancel/close ordering is covered for active connections with pending
  overlapped operations.
- The install/package consumer gate passes on Windows with `GameNet::core`.
- The workflow continues to validate the IOCP backend and does not introduce a
  select-style Windows backend.

## Non-Goals

This milestone does not promote HTTP, WebSocket, RPC, UDP, KCP, TLS, metrics,
or game pipeline modules. It only finishes the Windows backend for the current
Reactor / TCP foundation.
