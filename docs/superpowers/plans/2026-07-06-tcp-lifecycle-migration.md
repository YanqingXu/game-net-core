# TCP Lifecycle Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the pure TCP connection lifecycle chain without importing deferred modules.

**Architecture:** Extend `Callbacks.h`, then add plain `TcpConnection`,
`TcpServer`, and `TcpClient` under `gamenet::net`. Keep fd ownership,
Channel registration, and connection maps explicit and loop-owned.

**Tech Stack:** C++23, CMake, `GameNet::core`, existing Reactor primitives.

---

## File Structure

- Modify: `include/gamenet/core/net/Callbacks.h`
  Defines shared `TcpConnectionPtr`, connection/message/write/close callbacks,
  and keeps `ThreadInitCallback`.
- Create: `include/gamenet/core/net/TcpConnection.h`
  Public plain TCP connection lifecycle API.
- Create: `src/core/net/TcpConnection.cc`
  Socket/channel/buffer state machine and cross-thread marshaling.
- Create: `include/gamenet/core/net/TcpServer.h`
  Minimal acceptor/thread-pool/connection-map coordinator.
- Create: `src/core/net/TcpServer.cc`
  Accept, create, remove, stop, and cleanup paths.
- Create: `include/gamenet/core/net/TcpClient.h`
  Minimal connector/current-connection coordinator.
- Create: `src/core/net/TcpClient.cc`
  Active connect, disconnect, stop, and cleanup paths.
- Modify: `src/core/CMakeLists.txt`
  Register new source files.
- Modify: `tests/CMakeLists.txt`
  Register tests.
- Create: `tests/contract/tcp_connection/test_tcp_connection_lifecycle.cpp`
  Connection state/callback/cross-thread API contract.
- Create: `tests/integration/tcp/test_tcp_server_client_echo.cpp`
  Server/client echo path.

## Tasks

### Task 1: Write Failing TCP Contract Tests

- [ ] Add `tests/contract/tcp_connection/test_tcp_connection_lifecycle.cpp`
      referencing `TcpConnection` APIs before implementation exists.
- [ ] Add `tests/integration/tcp/test_tcp_server_client_echo.cpp`
      referencing `TcpServer` and `TcpClient` APIs before implementation exists.
- [ ] Register the tests in `tests/CMakeLists.txt`.
- [ ] Run the include resolver and confirm it fails on missing TCP headers.

### Task 2: Add Shared Callback Contracts

- [ ] Extend `Callbacks.h` with `TcpConnectionPtr`, `ConnectionCallback`,
      `MessageCallback`, `HighWaterMarkCallback`, `WriteCompleteCallback`,
      and `CloseCallback`.
- [ ] Keep callback definitions free of protocol, TLS, coroutine, metrics, and
      game-layer types.

### Task 3: Implement Plain TcpConnection

- [ ] Add the `TcpConnection` header with state, lifecycle, callbacks, send,
      shutdown, force-close, context, and address APIs.
- [ ] Implement owner-loop state mutation, buffer reads/writes, channel tie,
      close convergence, and remove-before-destroy.
- [ ] Keep high-water mark notification but do not implement backpressure
      read-pause policy in this stage.

### Task 4: Implement Minimal TcpServer

- [ ] Add the `TcpServer` header and implementation.
- [ ] Use `Acceptor` on the base loop, `EventLoopThreadPool` for worker loops,
      and a base-loop connection map.
- [ ] Close callbacks must return removal to the base loop through
      `EventLoop::runInLoop`.

### Task 5: Implement Minimal TcpClient

- [ ] Add the `TcpClient` header and implementation.
- [ ] Use `Connector` for active connect and create one `TcpConnection` after
      the fd is delivered.
- [ ] Public `connect`, `disconnect`, and `stop` marshal to the owner loop.

### Task 6: Verify And Commit

- [ ] Run `git diff --check`.
- [ ] Run the quoted-include resolver.
- [ ] Run the CMake source-list check.
- [ ] Scan for deferred modules:
      `MetricsHook|mini/|mini::|namespace mini|Broadcast|Tls|coroutine|Payload|HTTP|WebSocket|RPC|KCP|DnsResolver`.
- [ ] Attempt `cmake -S . -B build -DGAMENET_BUILD_TESTING=ON`.
- [ ] Commit as `feat(core): add tcp connection lifecycle`.
