# TCP Lifecycle Migration Design

## Goal

Complete the pure TCP core chain promised by the current roadmap:
`TcpConnection`, `TcpServer`, and `TcpClient`.

This stage keeps `game-net-core` focused on a small C++23 networking foundation
for game servers. It does not migrate higher-level protocol, transport,
observability, coroutine, TLS, DNS, broadcast, or game-session modules.

## Scope

This migration includes:

- shared TCP callback aliases in `Callbacks.h`
- plain `TcpConnection`
- minimal plain `TcpServer`
- minimal plain `TcpClient`
- unit, contract, and integration tests for lifecycle behavior

This migration excludes:

- TLS and TLS handshake events
- coroutine awaitables and cancellation tokens
- metrics hooks and exporters
- DNS resolution
- idle-timeout policy
- broadcast routing or payload pools
- HTTP, WebSocket, RPC, UDP, KCP, and game-session code
- protocol framing beyond raw bytes in `Buffer`

## Architecture

`TcpConnection` is the connection lifecycle center. It owns one `Socket`, one
`Channel`, an input `Buffer`, and an output `Buffer`. It borrows its owner
`EventLoop`. State transitions and buffer mutation occur only on that owner loop.

`TcpServer` owns an `Acceptor`, an `EventLoopThreadPool`, and a base-loop
connection map. Accepted fds are handed to the selected io loop as
`TcpConnection` objects. Close callbacks return removal to the base loop.

`TcpClient` owns one `Connector` and at most one established `TcpConnection`.
It supports explicit connect/disconnect/stop operations and safe close cleanup
without DNS, TLS, or multiple simultaneous sessions.

## Public Contract

- Connection callbacks fire on the connection owner loop.
- Message callbacks receive the connection and its input buffer.
- `send()` is cross-thread safe and marshals to the owner loop when needed.
- `shutdown()` waits for buffered output before half-closing the socket.
- `forceClose()` converges on the same close path as read EOF and errors.
- `connectEstablished()` ties the channel to shared connection lifetime and
  enables reading.
- `connectDestroyed()` disables and removes channel registration before
  destruction.

## Intent References

- `intents/modules/tcp_connection.intent.md`
- `intents/modules/tcp_server.intent.md`
- `intents/modules/tcp_client.intent.md`
- `rules/thread_affinity_rules.md`
- `rules/ownership_rules.md`
- `rules/testing_rules.md`

## Change Gate

- Owner loop/thread: each `TcpConnection` belongs to one owner `EventLoop`.
  `TcpServer` mutates its connection map on the base loop. `TcpClient` mutates
  client state on its owner loop.
- Ownership: `TcpConnection` owns socket/channel/buffers. `TcpServer` owns its
  acceptor and connection map. `TcpClient` owns its connector and current
  connection.
- Re-entry: user connection/message/write-complete callbacks may call
  `send()`, `shutdown()`, or `forceClose()`. These operations must remain
  loop-safe.
- Cross-thread operations: `TcpConnection::send`, `shutdown`, and `forceClose`
  marshal through `EventLoop::runInLoop` when needed. `TcpClient` public
  connect/disconnect/stop also marshal to the owner loop.
- Tests: contract tests cover `TcpConnection`; integration tests cover the
  server/client echo path.
