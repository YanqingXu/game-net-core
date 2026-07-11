# game-net-core

game-net-core is a modern C++23 networking foundation for game servers.

It is built around a Reactor-style EventLoop and aims to provide a small,
testable, and extensible base for game-server networking.

The repository is a component-by-component split and migration of the larger
`mini_trantor` project. The goal is to extract the networking foundation first,
stabilize it with clear ownership/threading contracts, and then promote higher
layers only after the core is proven.

## Migration Goal

The overall migration is staged:

1. Initialize the `game-net-core` project skeleton.
2. Migrate the Reactor / TCP core.
3. Split CMake targets and test structure.
4. Gradually migrate protocol, transport, game foundation, and experimental
   modules.

The Reactor / TCP foundation is frozen at `v0.1.0-core-preview`. Phase 4 now
adds independently targeted protocol, transport, session, logic, and broadcast
foundations without changing the core dependency direction.
The published integration/contract preview is
[`v0.2.0-phase4-preview`](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview);
it does not declare production readiness or API/ABI stability.
See `docs/migration_status.md` for the current phase status and verification
state.

## Current Scope

Stable / Core:

- Logger
- EventLoop
- Channel
- Poller
- TimerQueue
- Buffer
- InetAddress
- Socket
- Acceptor
- Connector
- TcpConnection
- TcpServer
- TcpClient
- EventLoopThreadPool

Phase 4 Foundations:

- `GameNet::protocol`: length-delimited PacketFramer
- `GameNet::transport`: TransportEndpoint and TCP adapter
- `GameNet::game_session`: PlayerSession and SessionManager
- `GameNet::game_logic`: bounded GameCommandQueue and LogicLoop
- `GameNet::broadcast`: owner-loop routing, bounded dispatch, and backpressure reasons

Planned / Deferred Modules:

- game packet headers and serialization codecs
- UDP/KCP experimental transport
- coroutine, TLS, HTTP, WebSocket, and RPC adapters

## Non-goals

This project is not:

- a full game server framework
- a production KCP implementation
- an AOI/world-state framework
- a database/cache framework
- a complete gateway platform

## Layout

```text
include/gamenet/   Public headers
src/               Implementation
tests/             Unit, contract, and integration tests
examples/          Minimal runnable examples
docs/              Architecture and scope notes
intents/           Intent-driven module and architecture contracts
rules/             Engineering rules for core behavior
```

## Examples

- `echo_server`: minimal raw TCP echo server built on `TcpServer` and `TcpConnection`.
- `echo_client`: minimal raw TCP echo client built on `TcpClient`.
- `game_server_pipeline_demo`: Phase 4 composition from framed TCP authentication
  through session/logic handling to a framed response. It is an example target,
  not an installed all-in-one pipeline library.

```bash
echo_server 7000
echo_client 127.0.0.1 7000 hello
```

## Development Workflow

game-net-core preserves the original intent-driven workflow:

```text
intent -> invariants -> threading -> ownership -> contracts -> tests -> implementation
```

Before changing a core module, read the matching file in `intents/` and the
relevant rules in `rules/`. Deferred intents are preserved for future phases,
but they do not expand the current implementation scope by themselves.

## Continuous Integration

The CI gate builds and tests the Reactor/TCP core plus active Phase 4 targets,
examples, Release configuration, and install/package consumer path. See
`docs/development/ci.md` for the workflow scope and local equivalent commands.
