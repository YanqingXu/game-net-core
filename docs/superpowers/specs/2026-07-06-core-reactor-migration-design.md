# Core Reactor Migration Design

## Goal

Migrate the low-level base and Reactor components from `mini_trantor` into
`game-net-core` without pulling in TCP server, TLS, coroutine, broadcast, RPC,
HTTP, UDP, or KCP modules.

## Scope

This migration includes:

- `Logger`, `Timestamp`, and `noncopyable`
- socket type and socket operation helpers
- platform wakeup helpers
- `Channel`
- `Poller`, `EPollPoller`, `IocpPoller`, and `PollerFactory`
- `TimerId` and `TimerQueue`
- `EventLoop`
- `EventLoopThread`
- `EventLoopThreadPool`

This migration excludes:

- `TcpConnection`, `TcpServer`, and `TcpClient`
- TLS
- broadcast routing and dispatch
- metrics exporter and game metrics
- coroutine awaitables
- HTTP, WebSocket, RPC, UDP, KCP, and DNS

## Architecture

Public headers are placed under `include/gamenet/core/base` and
`include/gamenet/core/net`. Implementations are placed under `src/core/base`
and `src/core/net`. Namespaces preserve the old conceptual split as
`gamenet::base` and `gamenet::net`, while include paths use the new
`gamenet/core/...` project identity.

`EventLoop` keeps only its own lightweight queue and wakeup metrics so it can
compile independently. TCP and game-facing metric types stay out of this batch.

## Testing

The first migrated tests cover:

- `Channel` callback dispatch and tie lifetime behavior
- `EventLoop` same-thread and cross-thread queue behavior
- `TimerQueue` one-shot, cancellation, repeating, and cross-thread scheduling

These tests form the contract boundary for the first core target.
