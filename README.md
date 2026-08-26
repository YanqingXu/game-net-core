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

M2 is closed. Exact promotion commit
`0c3012449ae36fa32656da33c4d1161f5129cde7` passed dual-platform CI,
sanitizers, capacity, benchmark, repeat-50, package consumers, and a fresh
uninterrupted 1h/3h endurance chain. The internally packaged result is
`v0.3.0-internal-candidate.1`; its package/SBOM/evidence identities are recorded
in [`docs/development/releases/v0.3.0-internal-candidate.1.md`](docs/development/releases/v0.3.0-internal-candidate.1.md).
That retained internal bundle remains the historical all-rights-reserved
candidate it was built as; it is not retroactively republished. M3 real gateway
integration is closed: the private gateway closure is
`0a8fe1e`, its exact uninterrupted 1-hour run used gateway `4e2457e` and Core
fix `736a090`, completed 3,743 full replay/fault cycles, and left no unresolved
Core correctness or reusable-capability blocker. M4 is now closed: exact
promotion commit `8e4a6edfe22ca43e3308e36ec31bf7f2dea14ac7` passed the complete
Linux/Windows, sanitizer, capacity, benchmark, repeat/fault, 1h/3h, package,
consumer, SBOM, and evidence matrix without a waiver. The stable Apache-2.0
[`v0.3.0`](https://github.com/YanqingXu/game-net-core/releases/tag/v0.3.0)
Release and its 12 canonical assets passed fresh-download verification. Exact
evidence and hashes are recorded in
[`docs/development/releases/v0.3.0.md`](docs/development/releases/v0.3.0.md).
M5 is closed with a second `NO-PROMOTION`: the independent gateway needed no
new broadly reusable Runtime capability, all Profile compositions remain
non-installed, and the
[load-selection guide](docs/architecture/runtime_profile_load_selection_guide.md)
is published without an empty v0.4 release. IOE-X11 is closed at exact
implementation checkpoint `013fecfe81277845eb3e60ccf5fe0205b753858d`, and
IOE-X12 is closed at `5be30e701c61f8d6700bcc4be6bc0ef152120fb8`.
IOE-X13 is closed at `5484d7a89b01597824bc860e4d2d3cf3cfd45a82`. IOE-X14 is
closed at `351b3c0e476a462265016d53361a02b5f2c51611` with one portable TCP
semantic suite across epoll, IOCP, and io_uring. M6/IOE-X15 is closed at
`43795e841ba2a279ed6a3d5d831d60a9f2a25570` with an explicit Linux-only
experimental installation surface and no stable-manifest drift. No preview tag
or Release was published. The M7-G0 readiness audit at
`44493b1d37c16567990e1660153d6b0843a8eecc` records external implementation
`DEFER` and shared RPC `NO-PROMOTION`: the gateway has no RPC consumer contract,
so no RPC/Lua package surface was added. Gateway M4 later closed at
`d03cacd5aead885fc61a419c71d5c32a060cb700`, and its external M7
intent/rules/contract names are authorized at
`92a26072c3300275edc9d069a59fc17913c7614c`. The external adapter was then
implemented at `e43393c85fa37604d340fe866610756c99f4fe4e` and closed at
`588acd079be93de3e230ba4f07dd111f7bec6a3c`: package-only dual-platform,
sanitizer, repeat, fuzz, true-TCP, and independent-consumer 8/8 evidence found
different wire, correlation, owner, and completion contracts. M7 is therefore
closed as `NO-PROMOTION`; Core RPC stays deferred with no RPC/Lua package
surface and no empty v0.6 release. M8 then compared Core
`e5ea9efa71dbe52e841423ec3cac3e9529158b22`, callback-only gateway
`588acd079be93de3e230ba4f07dd111f7bec6a3c`, and Actor-bound
`YanGameServer@b5254165389d762c3f3c63568c24ffab448fc501`. Core EventLoop/
TimerQueue 3/3 and YanGame async/coroutine/timer/RPC/persistence 9/9 passed,
but their result, executor, frame, resume, timer, and retirement contracts are
not substitutable. M8 is closed as `NO-PROMOTION`; all six async intents remain
deferred, and no coroutine/awaiter package surface or empty v0.7 release
exists. M9 then audited Core `fff4162`, gateway `588acd0`, and independent
`YanGameServer@b525416`: Core's active `TransportEndpoint` contracts passed
3/3, while the independent TLS fallback set passed 4/4 with two real-OpenSSL
tests explicitly skipped. Gateway has no TLS/WebSocket/DNS implementation and
YanGame's native-worker internal-RPC mTLS explicitly excludes GameNet
EventLoop TLS, so no substitutable two-consumer contract exists. M9 closes as
`NO-PROMOTION`; all five transport/protocol intents remain deferred, no empty
v0.8 release exists, and M10 UDP/KCP evidence review is the next governance
front. M10 then audited Core `f1f89f0`, gateway `588acd0`, independent
`YanGameServer@b525416`, and historical migration source
`mini_trantor@3eba368`. Both external consumers have no datagram runtime, all
five intents remain deferred, and the old source lacks the current typed,
generation-safe, bounded, dual-platform contract. M10 closes as
`NO-PROMOTION`; no experimental datagram package or empty v0.9 release exists,
and M11 v1.0 stabilization/release readiness becomes the final planned audit.
M11 then audited Core `a2977c9`; after rebuilding the current tree, Windows
Release passed 130/130, fixed-tick passed 20/20, Windows install consumers
passed 2/2, and v0.3 tooling contracts passed 10/10;
the reviewed v0.3 stable surface remains zero-diff. The repository still has
no v1 API/ABI policy, 0.3-to-1.0 consumer, same-commit dual-platform/
sanitizer/capacity/endurance evidence, or v1 artifacts. M11 therefore closes
as `DEFER` / `NO-RELEASE`: stable v0.3 remains current, no v1 tag or package is
created, and M1–M11 are fully adjudicated with no open governance front.
That sentence is the historical M11 closure state. On 2026-08-25 the project
opened a new performance-first HP0–HP8 line at governance baseline
`202bf9f993575d144c4c440a3972dd80733da0a7`. The unique promotion and integration
front is HP0: the default-off, non-installed `gamenet.hot_path_cost.v1` cost
ledger and fixed native Linux/epoll plus Windows/IOCP laboratory. Until both
clean exact-commit ledgers exist, governance permits at most one isolated
`EXPERIMENTAL-PRESTUDY` HP1-HP8 prototype in parallel. It must be default-off,
non-installed, non-exported and absent from the production Core path; it cannot
advance a milestone, switch a default, add an installed API, or mint promotion
evidence.
The first such slice, HP1 borrowed-view framing, is retained as
`KEEP-EXPERIMENTAL` after Windows Release differential and focused ASan
contracts plus development-only measurements. HP2 SPSC mailbox/source/outbox
is also retained as `KEEP-EXPERIMENTAL`: its Release/ASan contracts and zero
queue-allocation/lock/generic-post structure passed, but burst queue-age
regressed in the local development comparison. HP1 and HP2 remain planned;
neither changed the default/API path, and the single prestudy slot is released.
HP3's portable epoll slot-dispatch algorithm is retained with decision `DEFER`:
its contracts and structural O(1) comparison passed, but no native Linux epoll
path ran, so the large decoder-only signal is not promotion evidence.
HP4's fixed output-segment chain is also `DEFER`: focused Release/ASan and five
production regressions passed, and the model removed concatenation allocation/
copy, but its 1 KiB case regressed 65.10% while the 16 KiB case improved 94.07%.
It invoked no real writev/WSASend, so no production send or broadcast path changed.
HP5 owner-local credit leasing is retained as `KEEP-EXPERIMENTAL`: its focused
contracts and four production budget regressions passed, and the local model
reduced 1,600,000 shared atomic mutations to four with 92.86%+ median elapsed
improvement. It remains a single-owner model without real TcpConnection/cache
contention or fixed-lab evidence, so production admission is unchanged.
HP6-A adaptive phase quotas are `DEFER`: the synthetic model reduces maximum
round cost and control/lifecycle first-service delay, but increases rounds and
planner overhead and slightly regresses oldest age. HP6-B pools and HP6-C layout
are `SKIPPED-BY-EVIDENCE`; production EventLoop/storage remains unchanged.
HP7 is `SKIPPED-BY-EVIDENCE`: HP2 is not integrated and no in-scope flow has
two real asynchronous waits. Deferred coroutine intents now supersede unsafe
shutdown/arbitrary-thread resume semantics, but no runtime target or API exists.
HP8 backend promotion is `DEFER` / `NO-RELEASE` because no post-integration HP0
matrix exists. Additive CMake presets now provide PortableRelease,
NativeTunedRelease, PGO, Sanitizer and BenchmarkInstrumented workflows, plus a
deployment-tuning checklist; these tools do not change backend defaults or version.
Production Linux remains epoll, Windows remains IOCP, and the reviewed stable
API remains zero-diff. Historical
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

Linux-only experimental package (explicit opt-in):

- `GameNet::experimental_io_uring`: `IoUringTcpServer` and
  `IoUringTcpClient` façades with an independent experimental API manifest

Planned / Deferred Modules:

- game packet headers and serialization codecs
- UDP/KCP experimental transport
- coroutine, TLS, DNS, HTTP, WebSocket, and RPC adapters

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
Linux-only IOE-X1–X15 io_uring Engine/Pump/TCP/Hub/listener, semantic Adapter,
single-/multi-owner Server, active Client, cross-backend semantic contracts,
and benchmark tooling. It also installs the explicit
`GameNet::experimental_io_uring` component and Server/Client header closure;
it does not replace epoll, select a backend automatically, or enable deferred
transports. Windows rejects that option. Default packages contain no io_uring
target, library, or header.

See [Platform and Build Support](docs/development/platform_support.md) for the
support tiers, exact option behavior, commands, and Windows promotion criteria.

## Licensing Status

game-net-core is licensed under the [Apache License 2.0](LICENSE). The owner
authorized this transition on 2026-08-23; project attribution and dependency
boundaries are recorded in [NOTICE](NOTICE) and
[Third-Party Notices](THIRD_PARTY_NOTICES.md). Installed CMake packages include
those files and export `GameNetCore_LICENSE=Apache-2.0`. See
[Licensing Status](docs/development/licensing.md) and
[Release Packaging](docs/development/release_packaging.md). The authorized
transition and complete exact-commit evidence matrix produced the stable
[`v0.3.0` Release](https://github.com/YanqingXu/game-net-core/releases/tag/v0.3.0)
on 2026-08-23.

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
