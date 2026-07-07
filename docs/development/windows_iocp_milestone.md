# Windows IOCP Milestone

Last checked: 2026-07-07

Windows support is not promoted into CI for the Reactor / TCP foundation yet.
The current Windows source selection intentionally uses `IocpPoller`, and
WinSock `select()` is not an accepted fallback for the active target.

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

Full Windows CTest is still not a CI support gate, but a local VS2026 Debug run
with a 10-second per-test timeout now passes 29/29 tests with 0 failing tests.
The local IOCP evidence includes:

- `contract.event_loop.test_event_loop` and
  `contract.timer_queue.test_timer_queue`: `EventLoop::wakeup()` posts an IOCP
  completion packet through `PostQueuedCompletionStatus`, so cross-thread
  queued work and timer scheduling no longer sleep until the default poll
  timeout.
- `contract.acceptor.test_acceptor_contract`,
  `contract.tcp_server.test_tcp_server_contract`, and
  `contract.tcp_server.test_tcp_server_stop_active_connections`: the Acceptor
  path is backed by `AcceptEx`.
- `contract.connector.test_connector_contract` and
  `contract.tcp_client.test_tcp_client_contract`: active connects are backed by
  `ConnectEx` and preserve the IOCP socket association when the fd transfers
  from Connector to TcpConnection.
- `contract.poller.test_poller_contract`: the Windows Poller contract is backed
  by a posted IOCP read operation, not by a select-style fallback.
- `contract.tcp_connection.test_tcp_connection_lifecycle`,
  `contract.tcp_connection.test_tcp_connection_high_water_mark`,
  `contract.tcp_connection.test_tcp_connection_peer_close`,
  `contract.tcp_connection.test_tcp_connection_peer_reset`,
  `contract.tcp_connection.test_tcp_connection_shutdown_pending_output`,
  `contract.tcp_connection.test_tcp_connection_write_complete_ordering`, and
  `integration.tcp.test_tcp_server_client_echo`: TcpConnection read/write and
  lifecycle behavior are backed by loop-owned `WSARecv` / `WSASend`
  overlapped operations.

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

## Promotion Gates

Windows CI may be added only after all of these are true:

- `contract.event_loop.test_event_loop` and
  `contract.timer_queue.test_timer_queue` stay green on Windows without relying
  on the default poll timeout.
- IOCP wakeup has a direct contract that proves cross-thread queued work wakes
  the owner loop.
- Poller readiness/completion contracts are backed by posted IOCP operations,
  not by a select-style fallback.
- TcpConnection read, write, peer close/reset, shutdown-with-pending-output,
  and repeated teardown contracts pass through the IOCP data path.
- Cancel/close ordering is covered for active connections with pending
  overlapped operations.
- The install/package consumer gate passes on Windows with `GameNet::core`.
- Documentation and migration status still say Windows support is not promoted
  into CI until the remaining Windows packaging gate is green.

## Non-Goals

This milestone does not promote HTTP, WebSocket, RPC, UDP, KCP, TLS, metrics,
or game pipeline modules. It only finishes the Windows backend for the current
Reactor / TCP foundation.
