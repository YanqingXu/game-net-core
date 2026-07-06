# TCP Foundation Migration Design

## Goal

Migrate the TCP foundation pieces that sit below `TcpConnection`, `TcpServer`,
and `TcpClient`, while keeping deferred modules out of the current core target.

## Scope

This migration includes:

- `Buffer`
- `InetAddress`
- `Socket`
- `Acceptor`
- `ConnectorOptions`
- `Connector`

This migration excludes:

- `TcpConnection`
- `TcpServer`
- `TcpClient`
- TLS
- coroutine awaiters
- broadcast routing
- metrics exporter and game metrics
- HTTP, WebSocket, RPC, UDP, KCP, DNS, and game-session code

## Architecture

Public headers continue under `include/gamenet/core/net`. Implementations live
under `src/core/net`.

`Acceptor` and `Connector` stay thin fd adapters:

- `Acceptor` owns the listening socket and readable listen `Channel`, then
  forwards accepted fds through a callback.
- `Connector` owns one in-flight nonblocking connect attempt and forwards the
  connected fd through a callback.

Neither class creates `TcpConnection` or owns application protocol state.

## Intent References

- `intents/modules/buffer.intent.md`
- `intents/modules/acceptor.intent.md`
- `intents/modules/connector.intent.md`
- `rules/thread_affinity_rules.md`
- `rules/ownership_rules.md`
- `rules/testing_rules.md`

## Change Gate

- Owner loop/thread: `Acceptor` and `Connector` are owned by their configured
  `EventLoop` thread. `Buffer` and `InetAddress` have no internal thread owner.
- Ownership: `Socket` owns its fd unless `releaseFd()` transfers it. `Acceptor`
  owns listen socket/channel. `Connector` owns a connecting socket/channel until
  it transfers the connected fd upward or closes it.
- Re-entry: accepted/connected callbacks may call back into loop APIs, but they
  receive only an fd boundary in this stage.
- Cross-thread operations: `Connector::start()` and `Connector::stop()` marshal
  through `EventLoop::runInLoop`. `Acceptor::listen()` and `Acceptor::stop()` are
  owner-thread-only.
- Tests: `tests/unit/buffer/test_buffer.cpp`,
  `tests/unit/net/test_inet_address.cpp`,
  `tests/unit/socket/test_socket.cpp`,
  `tests/unit/acceptor/test_acceptor_stop.cpp`,
  `tests/unit/connector/test_connector.cpp`.
