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

The current active target remains the Reactor / TCP foundation.
See `docs/migration_status.md` for the current phase status and verification
state.

## Current Scope

Stable / Core:

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

Planned Modules:

- protocol framing
- transport abstraction
- session foundation
- logic loop
- broadcast foundation
- UDP/KCP experimental transport

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

The initial CI gate builds the current Reactor/TCP foundation, examples, CTest
suite, and install/package consumer path on Linux with deferred modules
disabled. See
`docs/development/ci.md` for the workflow scope and local equivalent commands.
