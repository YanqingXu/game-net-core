# game-net-core

game-net-core is a modern C++23 networking foundation for game servers.

It is built around a Reactor-style EventLoop and aims to provide a small,
testable, and extensible base for game-server networking.

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
- EventLoopThreadPool

Planned Modules:

- TcpConnection
- TcpServer
- TcpClient
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

## Development Workflow

game-net-core preserves the original intent-driven workflow:

```text
intent -> invariants -> threading -> ownership -> contracts -> tests -> implementation
```

Before changing a core module, read the matching file in `intents/` and the
relevant rules in `rules/`. Deferred intents are preserved for future phases,
but they do not expand the current implementation scope by themselves.
