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
The active Phase 6 line installs as `GameNetCore 0.3.0`. Its infrastructure
snapshot `b3443182d0606792df44a12bcb08927e767bc060` completed real 24-hour and
72-hour Linux endurance runs, but those runs predate the current implementation
checkpoint. M3-R1 is independently closed at `95a6ab5`, and M3-R2 is committed
at `12adb00`. API-R1 independently approved the remediated 0.3 stable Core
surface after closing its initial blockers. PERF-R1 then exposed deterministic
evidence-comparator, high-fd client, and Linux overload-profile defects in the
otherwise validated `v0.3.0-rel-c1-refreeze-1@944f7222d7aa7a36e12ffda4ad038ec3ae7d30d7`
candidate. The current implementation checkpoint is
`6b292156e3e94d3389e9f3b8513445e7eb4ab541` (`6b29215`); its single additive
stable API is source-compatible and bound to reviewed-surface tag
`api-r1-perf-r1-reviewed-surface`. REL-C1 now refreezes the unique v0.3
candidate through annotated tag `v0.3.0-rel-c1-refreeze-2`. The tag is an
engineering freeze reference, not a release tag or REL-D1 decision. The new
candidate requires fresh clean same-SHA CI, performance, capacity, and
endurance evidence; REL-V1 is the next task.
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

## Supported Builds

The current CMake target-system allow-list is Linux and Windows:

- Linux/epoll is Tier 1 and owns the release, sanitizer, performance, and
  long-duration reference evidence.
- Windows/IOCP is a required Tier 2 functional and package platform until the
  M3 batching, error-path, ownership, and capacity promotion gates complete.
- macOS, BSD variants, and other target systems fail during configure.

All installed targets are static-only before 1.0. `BUILD_SHARED_LIBS=ON` is
rejected, and no binary ABI compatibility is promised before 1.0.
`GAMENET_ENABLE_TLS` and `GAMENET_ENABLE_EXPERIMENTAL` are compatibility
options that currently accept only `OFF`; `ON` fails instead of silently
building no feature.

See [Platform and Build Support](docs/development/platform_support.md) for the
support tiers, exact option behavior, commands, and Windows promotion criteria.

## Licensing Status

The current top-level `LICENSE` is all-rights-reserved and grants no external
permission to use, copy, modify, or redistribute this code. Engineering
candidate work can continue, but an externally adoptable release is blocked
until the project owner publishes an explicit license and corresponding
package/SBOM metadata. See [Licensing Status](docs/development/licensing.md).

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
