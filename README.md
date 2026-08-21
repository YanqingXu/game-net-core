# game-net-core

game-net-core is a modern C++23 networking foundation for game servers.

It is built around a single-owner EventLoop scheduler/event pump with native
Readiness and Completion semantics, and aims to provide a small, testable, and
extensible base for game-server networking.

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
The active line installs as `GameNetCore 0.3.0`. M1 closed on 2026-08-22 with
IOE-X10 implementation/evidence checkpoint
`f5d39b800b4dd943531670aa09840c931c3dee4d`: a fixed 256-route listener
comparison produced a narrowly scoped `PROMOTE` for later source-private
io_uring shaping, and independent ARCH-G1 review concluded `APPROVE`. The
comparison does not claim that io_uring is faster overall and does not install
an io_uring target, open a public backend selector, or replace production
epoll. Evidence and review are recorded in
[`docs/development/benchmark_results/2026-08-22-ioe-x10-f5d39b8/evidence.json`](docs/development/benchmark_results/2026-08-22-ioe-x10-f5d39b8/evidence.json)
and [`docs/reviews/arch-g1-independent-review.md`](docs/reviews/arch-g1-independent-review.md).

The current implementation front is M2: select an exact promotion commit and
build the complete `v0.3.0-internal-candidate.1` evidence bundle before adding
new IOE or Runtime functionality. Production Linux remains epoll, Windows
remains IOCP, and the reviewed stable API remains zero-diff. Historical
API-R1/PERF-R1 and REL-C1 evidence remains immutable: implementation checkpoint
`669ebb0a7c5c475dea74b12275c66a2ce1876804`, reviewed-surface tag
`api-r1-perf-r1-reviewed-surface@6b292156e3e94d3389e9f3b8513445e7eb4ab541`,
annotated tag `v0.3.0-rel-c1-refreeze-5`, and superseded
`v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`.
These are engineering references, not current development gates or a release
decision.
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
`GAMENET_ENABLE_TLS` remains an `OFF`-only compatibility option. The default-off
`GAMENET_ENABLE_EXPERIMENTAL=ON` is accepted only on Linux and builds the
non-installed Linux-only IOE-X1–X10 io_uring Engine/Pump/TCP/Hub/listener
contracts and benchmark tooling; it does not replace epoll or enable deferred
transports. Windows rejects that option.

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
