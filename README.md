# game-net-core

A C++23 networking foundation for game servers, split component by component
from `mini_trantor`. A single-owner EventLoop combines native readiness on
Linux/epoll and completion on Windows/IOCP with explicit ownership, lifecycle
and backpressure contracts.

The library provides TCP networking plus separate protocol, transport, session,
logic and broadcast building blocks. Intent defines behavior; tests enforce
the contracts that implementations must preserve.

[Quick start](#quick-start) · [Components](#current-scope) ·
[KCP-C1–C4](#kcp-milestones) · [Validation](#continuous-integration) ·
[Documentation](#documentation)

## Current Status

The latest stable release is
[`v0.3.1`](https://github.com/YanqingXu/game-net-core/releases/tag/v0.3.1),
installed as `GameNetCore 0.3.1`. Its accepted-output graceful-shutdown fix and
package checks are recorded in the
[preflight report](docs/development/releases/v0.3.1-preflight.md).

The component descriptions below refer to the current `main` source tree.
Recent RPC, TLS, WS/WSS and UDP/KCP work has separate development authorization;
availability in a published package depends on that package's source revision
and build options. New development on `main` does not create a release.

KCP-C1 through KCP-C4 are implemented. C4 was merged in
[PR #36](https://github.com/YanqingXu/game-net-core/pull/36) at `009830a` on
2026-11-17, including bounded capacity, fault recovery and development endurance
validation. Its tool defaults OFF and is not installed or exported.

Formal HP0 performance promotion remains `DEFER`; the fixed-laboratory gate
is separate from feature correctness and development endurance. Current scope,
decisions and execution plans are tracked in [roadmap](docs/roadmap.md),
[migration status](docs/migration_status.md) and [plan](plan.md).

## Quick Start

Use CMake 3.20 or newer and a C++23 compiler. The Linux example also uses Ninja;
Windows requires MSVC and a Windows SDK. Use separate, fresh build directories
for optional features and sanitizer profiles.

Linux:

```sh
cmake -S . -B out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/linux --parallel 2
ctest --test-dir out/build/linux --output-on-failure --no-tests=error
```

Windows, from PowerShell:

```powershell
cmake -S . -B out/build/windows -A x64
cmake --build out/build/windows --config Release --parallel 2
ctest --test-dir out/build/windows -C Release --output-on-failure --no-tests=error
```

Tests and examples are built by default. The default CTest inventory is 133;
TLS, WebSocket, UDP, KCP, experimental io_uring, fuzz and benchmark tooling are
opt-in. Core-only builds do not require OpenSSL.

Install a completed Linux build with:

```sh
cmake --install out/build/linux --prefix out/install
```

On Windows, use `out/build/windows` and add `--config Release`. Set a consumer's
`CMAKE_PREFIX_PATH` to the resulting install prefix, then link the required
component:

```cmake
find_package(GameNetCore CONFIG REQUIRED COMPONENTS core)
target_link_libraries(my_server PRIVATE GameNet::core)
```

Optional components must be enabled when building the package and explicitly
requested by consumers, for example `COMPONENTS kcp_transport`. See
[release packaging](docs/development/release_packaging.md) and
[build profiles](docs/development/build_profiles.md) for consumer checks,
presets, sanitizer, native tuning and PGO workflows.

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
- `GameNet::broadcast`: owner-loop routing, bounded dispatch and backpressure reasons
- `GameNet::rpc`: bounded callback request/response, deadlines, cancellation and
  disconnect settlement; see [callback RPC](docs/development/callback_rpc.md)

Optional components on `main` (all options below default OFF):

| Component / tool | Build requirement | Contract and usage |
| --- | --- | --- |
| `GameNet::udp`, `GameNet::udp_transport` | `GAMENET_ENABLE_UDP=ON` | [Bounded datagrams](docs/development/udp.md), [explicit peers and endpoint](docs/development/udp_peer.md) |
| `GameNet::kcp`, `GameNet::kcp_transport` | `GAMENET_ENABLE_KCP=ON` and `GAMENET_ENABLE_UDP=ON` | [Standard KCP sessions, endpoint and examples](docs/development/kcp.md) |
| `GameNet::tls`, `GameNet::rpc_tls` | `GAMENET_ENABLE_TLS=ON`; OpenSSL 3.0+ | [TLS](docs/development/openssl_tls.md), [RPC/TLS](docs/development/callback_rpc_tls.md) |
| `GameNet::websocket` | `GAMENET_ENABLE_WEBSOCKET=ON` | [Bounded WS protocol and state validation](docs/development/websocket.md) |
| `GameNet::websocket_tls` | Both WebSocket and TLS options ON | [WSS composition and interoperability](docs/development/websocket_tls.md) |
| `GameNet::experimental_io_uring` | `GAMENET_ENABLE_EXPERIMENTAL=ON`; Linux only | Explicit experimental Server/Client component; native backend defaults remain epoll/IOCP |
| `gamenet_kcp_validation` | `GAMENET_BUILD_KCP_VALIDATION=ON`, KCP and UDP ON | [C4 validation](docs/development/kcp.md#kcp-c4-development-validation); non-installed, outside CTest |

KCP depends on Core/UDP; its endpoint adapter composes the separate transport
interface. Core and plain UDP/KCP/WS consumers remain independent of OpenSSL.
Default packages contain no disabled optional component headers or libraries.

Deferred work includes game-specific packet headers/business serialization,
coroutines, DNS, general HTTP services, PMTU and the historical custom KCP-style
design. Standard KCP and WSS have their own active implementations above.

## KCP Milestones

| Milestone | Implemented scope |
| --- | --- |
| C1 | Bounded standard KCP message sessions over explicit UDP peers, pinned MIT upstream, resource/lifecycle contracts and independent interoperability |
| C2 | `KcpTransportEndpoint` and finite server/client examples, preserving owner affinity, backpressure and terminal semantics |
| C3 | Real cross-loop integration with SessionManager, LogicLoop and BroadcastDispatcher, generation-safe bindings and exact shutdown settlement |
| C4 | 1/8/32-session IPv4/IPv6 capacity profiles, data/ACK blackholes, healthy-peer progress, recovery, timeout/replacement/history refusal and zero-residue cleanup |

Build KCP explicitly in a separate tree:

```sh
cmake -S . -B out/build/kcp -DCMAKE_BUILD_TYPE=Release -DGAMENET_ENABLE_UDP=ON -DGAMENET_ENABLE_KCP=ON
cmake --build out/build/kcp --config Release --parallel 2
ctest --test-dir out/build/kcp -C Release --output-on-failure --no-tests=error
```

Both ends must agree on known addresses and a fresh nonzero conv. Protocol ACK
means peer protocol reception; application processing is a separate contract.
Address/conv configuration is not authentication, and the examples do not add
discovery or an automatic reconnect policy. See the [KCP guide](docs/development/kcp.md)
for dual-family echo commands, ownership and close semantics.

C4's Python 3 runner builds the private tool and writes independently verified
`gamenet.kcp_capacity_endurance.v1` evidence. Use a fresh output directory for
each run:

```sh
python3 tools/run_kcp_capacity_endurance.py --profile release --build-dir out/build/kcp-c4 --output out/c4-smoke
python3 tools/run_kcp_capacity_endurance.py --profile release --mode soak --build-dir out/build/kcp-c4 --output out/c4-development-30m
python3 tools/run_kcp_capacity_endurance.py --verify-output out/c4-development-30m
```

On Windows, use `py -3` or your environment's Python 3 executable in place of
`python3`. Smoke runs six capacity/family profiles. Soak runs one N=32 process
for at least 1800 seconds, alternating IPv4/IPv6 cycles; run it on each platform.
`--profile asan` and Linux `--profile tsan` provide separate instrumented smoke.
The [C4 intent](intents/usecases/kcp_capacity_endurance.intent.md) defines the
limits, real PID/RSS observations, exact terminal ledgers and failure policy.

This is development evidence. Production capacity, HP0 performance promotion
and TCP candidate-1h/release-3h certification retain their separate gates.

## Examples

Run the TCP echo server and client in separate terminals after the default build:

```sh
./out/build/linux/examples/echo_server 7000
```

```sh
./out/build/linux/examples/echo_client 127.0.0.1 7000 hello
```

Windows binaries are `out/build/windows/examples/Release/echo_server.exe` and
`echo_client.exe`. The default build also provides `game_server_pipeline_demo`,
composing framed TCP authentication, sessions and logic handling in an example.
It is not an installed all-in-one pipeline library.

Optional builds add UDP socket/peer echo servers, bounded WS echo and KCP
server/client examples; use the corresponding [module guides](#documentation)
for address arguments, finite deadlines and shutdown behavior.

## Supported Builds

The current CMake target-system allow-list is Linux and Windows:

- Linux/epoll is Tier 1 and owns the release, sanitizer, performance, and
  long-duration reference evidence.
- Windows/IOCP is a required Tier 2 functional and package platform, including
  native completion ownership/cancellation, Debug/Release and consumer checks.
  Linux remains the fixed long-duration reference platform.
- macOS, BSD variants, and other target systems fail during configure.

All installed targets are static-only before 1.0. `BUILD_SHARED_LIBS=ON` is
rejected, and no binary ABI compatibility is promised before 1.0.
`GAMENET_ENABLE_TLS` is default OFF; ON builds the optional OpenSSL `GameNet::tls` component. The default-off
`GAMENET_ENABLE_EXPERIMENTAL=ON` is accepted only on Linux and builds the
Linux-only IOE-X1–X15 io_uring Engine/Pump/TCP/Hub/listener, semantic Adapter,
single-/multi-owner Server, active Client, cross-backend semantic contracts,
and benchmark tooling. It also installs the explicit
`GameNet::experimental_io_uring` component and Server/Client header closure;
it does not replace epoll, select a backend automatically, or enable deferred
transports. Windows rejects that option. Default packages contain no io_uring
target, library, or header.

See [Platform and Build Support](docs/development/platform_support.md) for the
backend tiers and platform contracts. The component table above and the
individual module guides describe the current optional feature flags.

## Continuous Integration

Native test inventories depend on the selected build:

| Configuration / gate | Tests |
| --- | ---: |
| Default | 133 |
| UDP + KCP | 143 |
| UDP + KCP + TLS + WS/WSS | 153 |
| Focused UDP/KCP sanitizer lane | 10 |
| Full Clang 18 TSan UDP/KCP gate | 143, plus allocator atomic/race controls |

C4 runs outside CTest: six native capacity/family profiles in each of the five
KCP platform/sanitizer lanes and the full Clang TSan job. Its evidence reader has
59 regression tests covering evidence rejection, process identity and clock behavior.
The KCP workflow also supports a manual 30-minute Release soak on both platforms.
Protocol and session fuzz each execute 20,000 units with seed 424242; independent upstream peers, installed
consumers and physical ON/OFF isolation supplement the native tests.

The [PR #36 checkpoint](https://github.com/YanqingXu/game-net-core/pull/36)
at `aa6f410` passed all eight workflows and 38 checks. Twenty official CI ZIPs
were independently read back on Windows and Linux. Its local Release development
runs completed 1801.609 seconds / 278 cycles on Windows and 1803.112 seconds /
293 cycles on Linux, with zero residue in every completed cycle. These results
belong to that validated feature checkpoint; the PR records source/binary
identity, scope and the CI fixes.

See [CI scope and commands](docs/development/ci.md),
[KCP validation](docs/development/kcp.md) and the
[KCP workflow](.github/workflows/kcp.yml) for exact inventory, sanitizer,
interoperability, fuzz and artifact-readback requirements.

## Documentation

- [Roadmap](docs/roadmap.md), [migration status](docs/migration_status.md) and
  [execution plan](plan.md)
- [Intent catalog](intents/README.md) and [engineering rules](rules/)
- [Runtime Profile selection](docs/architecture/runtime_profile_load_selection_guide.md)
  and [build profiles](docs/development/build_profiles.md)
- [Callback RPC](docs/development/callback_rpc.md), [RPC/TLS](docs/development/callback_rpc_tls.md)
  and [OpenSSL TLS](docs/development/openssl_tls.md)
- [UDP](docs/development/udp.md), [UDP peers](docs/development/udp_peer.md)
  and [KCP](docs/development/kcp.md)
- [WebSocket](docs/development/websocket.md) and [WSS](docs/development/websocket_tls.md)

## Development Workflow

game-net-core preserves the original intent-driven workflow:

```text
intent -> invariants -> threading -> ownership -> contracts -> tests -> implementation
```

Before changing a core module, read the matching file in `intents/` and the
relevant rules in `rules/`. Deferred intents are preserved for future phases,
but they do not expand the current implementation scope by themselves.

## Non-goals

The project provides networking building blocks rather than a full game server,
AOI/world-state system, database/cache framework or complete gateway platform.
Provisional component validation does not certify a production deployment.

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
src/               Component implementations
tests/             Unit, contract, integration and engineering checks
examples/          Minimal runnable compositions
benchmarks/        Opt-in performance and development validation tools
tools/             Evidence runners and engineering utilities
docs/              Architecture, usage, scope and validation records
intents/           Module and use-case contracts
rules/             Threading, ownership, lifecycle and testing rules
```

## Historical Checkpoints

The records below describe the migration and promotion decisions at their
original checkpoints. Current independently authorized components are listed
above; the [migration status](docs/migration_status.md) preserves the full evidence.

<details>
<summary>Migration, release and HP0–HP8 history</summary>

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

The latest stable release is
[`v0.3.1`](https://github.com/YanqingXu/game-net-core/releases/tag/v0.3.1). It
installs as `GameNetCore 0.3.1` and includes the accepted-output
graceful-shutdown fix. The test and package checks are recorded in the
[preflight validation report](docs/development/releases/v0.3.1-preflight.md).

M1 closed on 2026-08-22 with
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
closed as `NO-PROMOTION`; at that checkpoint Core RPC remained deferred with no RPC/Lua package
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
v0.8 release existed, and M10 UDP/KCP evidence review was the next governance
front. M10 then audited Core `f1f89f0`, gateway `588acd0`, independent
`YanGameServer@b525416`, and historical migration source
`mini_trantor@3eba368`. Both external consumers have no datagram runtime, all
five intents remain deferred, and the old source lacks the current typed,
generation-safe, bounded, dual-platform contract. M10 closes as
`NO-PROMOTION`; no experimental datagram package or empty v0.9 release exists,
and M11 v1.0 stabilization/release readiness became the final planned audit.
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
On 2026-10-13, clean commit `41ba8f4ebeda784ff2952b87e53b48310d4f053f`
completed the graceful output-drain fix, passed all seven jobs in
[CI run 38336714098](https://github.com/YanqingXu/game-net-core/actions/runs/38336714098),
and produced independently validated Windows/IOCP and WSL Ubuntu 24.04/epoll
development ledgers: all six original scenarios, one warmup plus ten recorded
samples each, unchanged 10k capacity, and 132 retained child executions total.
Each ledger still lacks 380 required sample-cost observations and has
`promotion_eligible=false`. Native fixed Linux, complete observers, and reviewed
CPU controls remain missing, so formal HP0 stays `DEFER`. Evidence belongs to
that exact commit, not later documentation revisions. See the
[validation record](docs/development/2026-10-13-graceful-drain-validation.md).

The user-directed local Windows/WSL execution is now closed with `DEFER`.
[PR #15](https://github.com/YanqingXu/game-net-core/pull/15) is merged at
`af01a814f758aa6b02fa1bfed95810d8495ed58e`; its
[main CI](https://github.com/YanqingXu/game-net-core/actions/runs/38341614323)
passed 7/7 jobs, and the
[manual prestudy run](https://github.com/YanqingXu/game-net-core/actions/runs/38341836902)
passed all five lanes and 35/35 contracts, with artifact digests and evidence
independently verified. The weekly workflow is `active` with cron `23 3 * * 1`.
The initial verified execution used `workflow_dispatch`. As of 2026-11-10,
four subsequent `schedule` runs succeeded on `a4d4af64`: October 19, October 26,
November 2, and [November 9](https://github.com/YanqingXu/game-net-core/actions/runs/42556746551).
These five-lane, seven-contract-per-lane prestudy runs verify development
contracts; they do not complete HP0 or constitute release endurance evidence.
This deployment and software validation does not transfer the
`41ba8f4` benchmark measurements to the merge or later documentation commits.
Formal HP0 remains a future promotion gate.

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

</details>
