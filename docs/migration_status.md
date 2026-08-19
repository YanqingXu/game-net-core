# Migration Status

Historical audit field preserved by contract — Last checked: 2026-07-11

Phase 4 Preview publication checked: 2026-07-12

Current production-roadmap audit: 2026-08-20

Historical implementation checkpoint carried by the former candidate:
`669ebb0a7c5c475dea74b12275c66a2ce1876804` (2026-08-18)

Superseded PERF-R1 candidate: `v0.3.0-rel-c1-refreeze-4` peeled to
`c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`. Its detached REL-V1, REL-V2 run
`32043448820`, and paired Core benchmark run `32043874669` passed. Capacity run
`32043877128` failed twice on hosted Windows: one probe in attempts 1 and 2
closed on its client I/O deadline before the corresponding server accept was
published, so exact accept accounting could no longer converge. Both raw
failure documents remain historical evidence.

Historical v0.3 engineering candidate: the commit peeled from annotated tag
`v0.3.0-rel-c1-refreeze-5`; the tag object and remote ref record the
immutable historical `CANDIDATE_SHA`. The tag remains an ancestor of the
checked-out commit, but it is no longer a development freeze point and current
runtime/source progress is allowed to advance beyond it. New evidence binds the
exact commit it validates; the historical tag is never moved or reused.

Current M3-R1 independent-review checkpoint: `95a6ab5` (2026-08-03)

Current M3-R2 committed contract checkpoint: `12adb00` (2026-08-05)

Current post-review TCP establishment-remediation checkpoint: `9d2a5be`
(2026-08-11)

Current RTM-R2 Profile D MultiIoShardedHybrid checkpoint:
`b3b184b1cd8d28a256cffc60679ab832373dcfef` (2026-08-20)

Current API-R1 surface decision: `APPROVE` (2026-08-05); reviewed snapshot is
bound to annotated tag `api-r1-approved-surface` and peeled commit
`9d2a5be0eb5439399f27c2f53ec1bf985c7de1d0`

Current PERF-R1 additive stable-surface decision:
`approved-additive-source-compatible` (2026-08-17); reviewed snapshot is bound
to annotated tag `api-r1-perf-r1-reviewed-surface` and peeled commit
`6b292156e3e94d3389e9f3b8513445e7eb4ab541`

Current IOE-R1 source-private Engine checkpoint:
`8bb14e72d8935879396d12a7a51c891311aa2a78` (2026-08-19)

Current IOE-R2 generation-safe Readiness checkpoint:
`6f45aa6e78152b8fd86df925962e580101b2f2ee` (2026-08-19)

Current IOE-C1 typed operation-model checkpoint:
`f407440084c48f324ab3a65912443bb257aa8e77` (2026-08-19)

Current IOE-C1 direct read/write checkpoint:
`82d89831de81522ecd25c8eedd42f395e2a07613` (2026-08-19)

Current IOE-C1 all-kind direct consumer checkpoint:
`d0f2e07c3e3798605ac5d60ee33a7f7689fd542c` (2026-08-20)

Current IOE-C1 compatibility-path and shutdown closure checkpoint:
`c2d7e9d65dab8b110b58c20594b0cebd5199cf26` (2026-08-20)

Current RTM-R1 Profile A SingleLoopInlineEvent checkpoint:
`adb8b483d9b00ed0e9723321f2d7438e43a5e478` (2026-08-20)

Current RTM-R1 Profile B MultiIoQueuedEvent checkpoint:
`633d61315a9e28db643ee91214dc2f26a9b64630` (2026-08-20)

Current RTM-R1 Profile C MultiIoDedicatedFixedTick checkpoint:
`da57edc887421503e93ca743afb8a3373642c878` (2026-08-20)

## Current Task Goal

`game-net-core` is the component-split migration target for the larger
`mini_trantor` project. ARCH-G1 now has active I/O Engine and Runtime Model
intents, an accepted ADR, a semantic-coupling inventory, a concrete test map,
and a dual-platform local baseline. IOE-R1's source-private Engine contract,
typed results, option layering, Poller adapter, and shutdown/dispatch contracts
are closed at `8bb14e72`. IOE-R2's generation-safe epoll Readiness Engine,
internal wakeup, bounded wait, and current-interest/stale-token contracts are
closed at `6f45aa6e`. IOE-C1 native Completion contracts are closed at
`c2d7e9d6`: the operation model is committed at `f4074400`, direct read/write
at `82d8983`, and all-kind direct consumption at `d0f2e07`. All four operation
kinds keep kernel-terminal and consumer-terminal state separate and lease only
source-private driver/pool/attempt storage. The closure removes the remaining
completion-to-Channel publisher, operation publication link, Channel storage
implementations and Acceptor fallback; inherited Windows `Poller::poll()`
rejects use. The four shutdown phases are observably monotonic, and real
cancellation terminal consumption occurs during Quiescing. Windows
Debug/Release, Linux ASan/UBSan, repeated focused contracts, scope/API guards,
directional performance smoke, exact-commit tests, and remote-main verification
all pass. RTM-R1 Profile A is closed at `adb8b483`: its non-installed
`SingleLoopInlineEvent` composition runs TCP accept, connection ownership,
framing, bounded inline handling, output admission, and shutdown on one
caller-owned EventLoop with zero TcpServer workers and zero cross-domain
handoffs. Queue saturation, handler overrun/exception, protocol/output failure,
callback-reentrant stop, and bounded shutdown are terminal and observable.
Profile B is closed at `633d613`: its non-installed `MultiIoQueuedEvent`
composition uses two network owners, a distinct logic owner, bounded
GameCommandQueue handoff/drain, generation-safe owner return, typed saturation
recovery, and separate network/logic stop futures. Profile C
is closed at `da57edc`: its non-installed `MultiIoDedicatedFixedTick`
composition uses two network owners, one distinct logic/tick owner,
authoritative fixed-rate skip/bounded-catch-up cadence, bounded per-tick queue
drain, generation-safe owner return, and separate network/logic stop futures.
Profile D is closed at `b3b184b1`: its non-installed
`MultiIoShardedHybrid` composition separates connection placement from stable
player/room/scene sharding across at least two bounded logic cells, preserves
cell-local order across event and fixed-tick lanes, keeps established network
owners immobile, and isolates per-cell saturation. IOE-X1 one-shot completion
is closed at `d3b31c5`: the Linux-only non-installed raw io_uring Engine proves
Accept/Recv/Send, typed SQ-full, cancel/terminal lease, final drain, zero
fallback, sanitizer execution, and structured directional numbers while epoll
remains the production default/fallback. The cross-Profile real-TCP contract
now drives the same echo/stop lifecycle through A/B/C/D and closes the
common-capability review as `NO-PROMOTION`; repeated execution also closes the
Profile C cadence-stop summary publication race. The active implementation
front is IOE-X2 contract shaping; independent ARCH-G1 review proceeds in
parallel. Candidate freeze, REL-V1 and release packaging are not development
prerequisites.

The IOE-C1 closure checkpoint's directional Windows Release echo check used
4 connections, one
I/O thread, 10,000 messages per connection, 256-byte payloads, a 500 ms settle
window, and a 30 s timeout. Across five local samples, the medians were
124,021.278 round trips/s, 60.5573 MiB/s, and 68.1 us P99. The matching five-
sample 256-connection check reported 15.7157 ms establishment, 16,289.443
connections/s, 12,208 bytes/connection, 6.8434 ms close, and 0.2995 ms server
stop medians. Echo throughput is 4.43% above the immediately preceding local
all-kind-direct snapshot. Connection-establishment throughput is 13.15% lower,
while close latency and retained bytes/connection improve; this unpaired local
smoke has no regression threshold and does not claim a cross-commit performance
decision. All ten runs report `status: ok`. These numbers are same-worktree
smoke signals only, not promotion evidence.

The Profile A minimum-handoff Windows Release baseline uses the existing Core
echo probe with 4 connections, zero TcpServer worker loops, 10,000 messages per
connection, 256-byte payloads, a 500 ms settle window, and a 30 s timeout. Five
serialized samples produced medians of 122,760.126 RTT/s, 59.9415 MiB/s, 63.6 us
P99, 0.1748 ms connection close, 0.1297 ms server stop, and 184,320 bytes working-
set delta. This is a directional topology baseline, not a promotion threshold;
the real framed Profile contract separately proves exact 2/2/1 bounded dispatch,
same-owner queued continuation, and zero cross-domain handoffs.

The Profile B directional Windows Release baseline uses 4 connections, 2
TcpServer worker loops, 1 distinct logic loop, 5,000 messages per connection,
256-byte payloads, and 64-message client batches. Ten serialized samples at
`633d613` all reported `status: ok`; median throughput was 362,545.500 messages/s
(88.512 MiB/s), network-to-logic P99/P999 was 96/128 us, logic-to-network
P99/P999 was 64/128 us, maximum queue age was 162 us, queue-depth high-water was
127.5, working-set delta was 1,656,832 bytes, and shutdown was 1.955 ms. Every
sample reported exactly 40,000 cross-domain handoffs and producer wake posts
plus merges equal to the 20,000 accepted commands. These are directional local
numbers, not promotion evidence.

The Profile C directional baseline uses 4 clients, 2 network owners, 1 logic
loop, 2,000 messages per client, 256-byte payloads, a 1 ms fixed-rate cadence,
bounded catch-up of 1, and at most 1,024 commands per tick. Ten serialized
Windows Release samples all reported `status: ok`; medians were 127,956.083
messages/s (31.2395 MiB/s), 1,024 us tick-jitter P999, 256 us tick-duration
P999, 1,024 us queue-age P999, 0 skipped ticks, 29.5 catch-up ticks, queue-depth
high-water 256, 1,353,728 bytes working-set delta, and 1.939 ms shutdown. Ten
serialized WSL Ubuntu Release samples also all reported `status: ok`; medians
were 5,643.996 messages/s (1.378 MiB/s), the same three histogram buckets,
0 skipped ticks, 168 catch-up ticks, queue-depth high-water 256, 1,366,016
bytes working-set delta, and 1.3665 ms shutdown. Every sample reported exactly
16,000 cross-domain handoffs. The Windows/WSL throughput difference is a
directional IOCP-versus-WSL/DrvFS epoll observation, not a native-platform
regression decision or promotion threshold.

The Reactor / TCP foundation remains frozen at `v0.1.0-core-preview`. Phase 4
protocol, transport, session, logic-loop, pipeline-example, and broadcast
foundations are now implemented as one-way upper layers. Experimental
UDP/KCP/TLS/coroutine and HTTP/WebSocket/RPC adapters remain deferred.

The historical implementation checkpoint is `669ebb0`. M3-R1 is
closed at independently reviewed checkpoint `95a6ab5`; M3-R2 is committed at
`12adb00`; API-R1 remediation is committed at `7fa6922`; post-review TCP
establishment rollback remains at `9d2a5be`. PERF-R1 retains its reviewed
owner-loop-only send-buffer request and fail-closed v1-to-v2 bridge, then adds
annotated-tag checkout, warm paired/interleaved collection, one retention
snapshot batch per owner loop, complete JSON flush, byte-preserving stderr
diagnostics, and a connect/accept/echo/close batch barrier that leaves the
reviewed two-second probe deadline unchanged. The
inventory is 129 configured CTest tests: 8 unit tests, 107 contract tests, and
14 integration tests, with 102 threading and 107 lifecycle labels. Complete local
`candidate-10k` preflight for the barrier passed nine times on Windows and
three times on Linux with identical profile parameters; local Windows
regression/Core-capacity paired matrices pass their original budgets. None is
immutable refreeze-5 evidence. REL-C1 historically recorded the remediation
through `v0.3.0-rel-c1-refreeze-5`; it no longer defines the next roadmap task.
Local/remote tests and PERF-R1-style measurements now run continuously against
the commit under review, while endurance remains a promotion-only gate.

Direct closure contracts are
[`test_tcp_server_establishment_saturation.cpp`](../tests/contract/tcp_server/test_tcp_server_establishment_saturation.cpp),
[`test_tcp_client_contract.cpp`](../tests/contract/tcp_client/test_tcp_client_contract.cpp),
[`test_event_loop_thread_pool.cpp`](../tests/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp),
and [`test_tcp_server_contract.cpp`](../tests/contract/tcp_server/test_tcp_server_contract.cpp).

All candidate, benchmark, capacity, and endurance records below that name
`be749ad`, `5f926f3`, `b344318`, or `944f722` are historical evidence. They validate the
tooling or an earlier code checkpoint, not the current refrozen candidate;
final promotion requires complete same-SHA evidence.

The post-Preview production-hardening line is now active. Its first completed
contracts suppress Linux per-write `SIGPIPE`, bound per-connection admitted
output bytes across owner/cross-thread sends, return explicit overload results,
cap unread input buffers, apply owner-loop high/low-water read throttling, and
publish completion-aware graceful server drain results with a forced timeout fallback.
Recoverable listener/connection socket setup now returns explicit errors, and
Acceptor/TcpServer expose an owner-loop Retry-or-Stop runtime error policy.
Asynchronous EventLoop callbacks now have observable Continue/Quit exception
containment, thread-init failures return through the startup handshake, and a
throwing TCP business callback closes only its offending connection while
server bookkeeping and later admission continue.
TcpServer now optionally enforces global/per-peer connection limits, a bounded
per-peer fixed-window attempt rate, and base-loop unauthenticated deadlines;
all are disabled by default and expose distinct cumulative metrics.

The historical M1 worktree closed the local runtime lifecycle slices in
dependency order. Each EventLoop owns a dynamic lifecycle hub with allocation-
free cross-thread signals and generation-safe detach; loop shutdown now moves
through quiescing and final draining and, on IOCP, waits for lifecycle and
completion silence. TcpConnection has explicit socket-close/completion-drain
phases and first-writer structured close reasons. TcpServer stop is aggregated
per worker with a worker-cleanup, base-bookkeeping, BaseReleased, worker-ack,
and join handshake. TcpClient exposes a detached-lifetime-safe
TcpClientControl mailbox. These are local implementation and contract results,
not final frozen-SHA release evidence.

The historical M2 worktree aligned the provisional Phase 4/5 upper layers with
those Core contracts. Session and Pipeline handoffs now return typed terminal
results, exact session-binding generations suppress superseded commands and
outputs, the Pipeline applies Core admission/backpressure/authentication and
graceful-stop APIs, and heartbeat-driven idle expiry is wired through a real
TCP integration. Broadcast consumes immutable targets and enforces cross-plan
per-owner/global outstanding budgets with explicit terminal reason counts.
The opt-in Phase 4 harness now separately measures O(N) session scan-only and
full-expiry cleanup at 10k/100k/1M scales before any deadline-index redesign.
These remain worktree results until final Linux ASan/UBSan, TSan,
cross-platform, and frozen-SHA evidence is attached.

The historical initial M3 worktree started PR-G with one bounded Windows IOCP slice.
`IocpPoller` now drains `GetQueuedCompletionStatusEx` in fixed batches of at
most 64 packets. Wakeup packets do not truncate real I/O already present in
the batch, different Channels may be published together, and extra
completions for the same Channel are deferred to a later poll round in
Poller-owned fixed storage while their operation leases remain retained. This
avoids a read callback registration-generation change suppressing a same-round
write callback and stranding completion state. The local Windows MSVC Debug
tree builds in full, all 110 CTest tests pass, the focused IOCP lifecycle slice
passes 8/8, and the Debug benchmark reports
`get_queued_completion_status_ex_batch_64`. This is a local implementation
gate only: wakeup coalescing, buffer/segment ownership, AcceptEx pooling,
Release performance comparison, and frozen-SHA cross-platform evidence remain
open.

M3-L0 now closes the AcceptEx/ConnectEx immediate-quit final-drain gap.
Successfully submitted pending operations keep storage retention separate from
an allocation-free, idempotent shutdown obligation. Acceptor and Connector mark
that obligation before cancellation; `ERROR_NOT_FOUND` cannot bypass the real
queued packet, while synchronous non-pending submission failures create no
phantom obligation. The Windows MSVC Debug worktree passes the focused IOCP
lifecycle slice 8/8, the complete lifecycle gate 89/89, the full CTest inventory
110/110, and all 34 repository guards. These remain local development-gate
results rather than Release, sanitizer, performance, or frozen-SHA evidence.

M3-R1 is closed at candidate
`95a6ab5afbe33c4f84ab11c926e4867da94e8282`. The first candidate `446f86d`
was independently rejected because partial TcpConnection construction could
close an accepted fd while the base `pendingSocket` still owned the same
numeric value. The approved remediation performs every fallible constructor
step before the connection Socket claims the fd, completes construct/store/
release under the pre-armed participant boundary, and adds deterministic
construction-failure coverage. A Codex independent reviewer verified all 11
matrix rows from a clean WSL2 checkout; Linux/epoll Debug and Release each
passed focused 50/50 and full 120/120. Same-SHA GitHub run `30813037693`, attempt
2, passed Linux Debug/Release/TSan/ASan-UBSan and Windows Debug/Release. The
aggregate artifact remained unavailable (0 retained artifacts), so P1-02
release-evidence work remains open even though M3-R1/P1-01 is closed.

M3-R2 closes the EventLoopThreadPool configuration-state contract at
checkpoint `12adb00`. The explicit lifecycle is `Idle -> Started -> Idle`:
negative thread counts, non-base-loop configuration, configuration after
start, and repeated start all fail before mutation; idle stop remains
idempotent, and stop followed by reconfiguration/restart remains legal.
Zero-thread startup still invokes the base-loop initializer exactly once,
partial-start rollback remains covered, and TcpServer forwards negative and
late thread-count rejection. The new negative contracts failed against the
old implementation. After the fix, Windows/IOCP Release passed 120/120 CTests,
36/36 repository/API/CI guards, and 150/150 focused repetitions across the
pool contract, restart soak, and TcpServer contract. This is local worktree
evidence, not a frozen-SHA or remote release claim.

Phase 6 production-candidate infrastructure is now integrated on the roadmap
branch for audit. The compatibility boundary is a v2 installed-header/target
manifest with a retained `v0.2.0-phase4-preview` snapshot and deterministic
structural diff: stable Core, provisional Phase 4/Metrics, and unsupported
platform-backend interfaces remain distinct, while ABI compatibility stays
out of scope before 1.0. The candidate package version is `0.3.0`, and the
external install consumer continues to require that exact version.
MetricsExporter and producer adapters are implemented as provisional
observability APIs; their string/hash/global-mutex reference implementation is
not yet a promoted hot-path contract. The Core benchmark now emits
`gamenet.core_benchmark.v2`, uses `trySend()`, and validates requested,
accepted, rejected, hard-limit, pending-output, pause/resume, and recovery
semantics. The fixed Core/Phase-4 matrix retains the historical v1 executable
only as a performance baseline and compares common metrics on the same runner.
The production fault-injection executable covers reset, callback failure,
bounded-output rejection, post-failure recovery, and slow-reader forced drain
in one long-lived-process cycle. Its supervisor fixes candidate/release modes
at 24/72 hours, writes monotonic checkpoints and hashed evidence, and requires
the 72-hour run to consume same-SHA 24-hour evidence. Infrastructure snapshot
`b3443182d0606792df44a12bcb08927e767bc060` completed the real Linux/epoll
24-hour run `29895457789` (73,617 cycles) and 72-hour paired run
`29984629032` (220,851 cycles). Its cross-platform performance run
`29808395220` passed at earlier SHA `5f926f3`; these are historical
infrastructure-validation records, not evidence for later runtime changes.
For owner-authorized omission of long-duration evidence, the manual workflow
now also exposes `candidate-waiver` and `release-waiver` paths. They revalidate
the same frozen commit's 10k or dedicated 100k capacity pair on a hosted runner
and record owner/reason metadata with `status: waived`; neither is represented
as a passing 24/72-hour result.

## Phase Status

| Phase | Scope | Current status |
| --- | --- | --- |
| 1 | Initialize the `game-net-core` project skeleton | Present: top-level CMake, README, AGENTS, docs, intents, rules, include/src/tests/examples layout |
| 2 | Migrate Reactor / TCP core | Present: base utilities, socket helpers, Channel/Poller/EventLoop/TimerQueue, Acceptor/Connector, TcpConnection/TcpServer/TcpClient |
| 3 | Split CMake targets and test structure | Present: `gamenet_core`, `GameNet::core`, install/export package config, echo examples, unit/contract/integration test directories, scope/intent/documentation guards, install consumer fixture, an opt-in core benchmark target, and Acceptor/Buffer/Channel/Connector/InetAddress/Poller/Socket/TcpClient/TcpServer/TcpConnection/EventLoopThread/EventLoopThreadPool contract tests |
| 4 | Gradually migrate protocol / transport / game foundation / experimental | Foundation merged and published as `v0.2.0-phase4-preview`: PacketFramer, TransportEndpoint/TCP adapter, PlayerSession/SessionManager, bounded LogicLoop queue, pipeline demo/integration, and broadcast/backpressure; experimental transports remain deferred |
| 5 | Production hardening | M3-R1/M3-R2, API-R1 remediation, TCP establishment rollback, and the PERF-R1 probe-lifecycle remediation at `669ebb0` are historical foundations. Frozen-candidate requalification no longer blocks new capability work; validation follows each exact commit |
| 6 | Promotion infrastructure | Historical REL-C1 tag `v0.3.0-rel-c1-refreeze-5` replaced `v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`. API diff, metrics, regression, capacity, fault injection, endurance and waiver infrastructure remain available as continuous or promotion-only gates |
| 7 | I/O Engine and Runtime Profiles | Active: ARCH-G1 artifacts are complete with independent review pending; IOE-R1 is closed at `8bb14e72`, IOE-R2 at `6f45aa6e`, IOE-C1 at `c2d7e9d6`, and RTM-R1 Profiles A/B/C at `adb8b483`/`633d613`/`da57edc`. RTM-R2 Profile D is closed at `b3b184b1` with independent placement/sharding and cell-local Hybrid ordering. IOE-X1 is closed at `d3b31c5` with a Linux-only real one-shot Engine, bounded terminal lifecycle, zero fallback, sanitizer evidence, and an opt-in benchmark. The cross-Profile real-TCP contract closes common-capability review as `NO-PROMOTION`; all four Profiles and IOE-X1 remain non-installed. The active front is IOE-X2 contract shaping. No candidate freeze is required; each integrated slice carries exact-commit contracts and evidence |

## Current Intent Inventory

This is the only current intent inventory in this document. The migration-status
contract derives it from `intents/README.md`, every formal intent's front
matter, and active-body verification paths; checked-in numeric literals are not
the source of truth.

| Formal | Active | Deferred | Legacy | Explicit verification paths |
| ---: | ---: | ---: | ---: | ---: |
| 67 | 36 | 20 | 11 | 179 |

## Historical Production-Hardening Evidence

The entries in this section are chronological implementation and validation
records. They are not current-candidate claims unless the record explicitly
names the same SHA as the authoritative checkpoint above.

- The EventLoopThreadPool configuration-state contract now enforces
  non-negative thread counts, base-loop ownership, immutable started
  configuration, repeated-start rejection, idempotent idle stop, and legal
  stop/reconfigure/restart. Its negative and TcpServer forwarding contracts
  pass the current Windows/IOCP Release local gate; candidate freeze and
  same-SHA remote requalification remain pending.
- M3 PR-G completed its then-current local IOCP batching slice: fixed-size
  GQCSEx collection, bounded per-round publication, same-Channel deferral,
  wakeup coexistence, and exact outstanding-operation retention. IOE-C1 later
  replaced the raw deferred storage with typed terminal notices and later
  removed completion-to-Channel coalescing entirely.
- M3-G2 adds a Poller-owned atomic wakeup-pending bit: one false-to-true
  producer posts the physical IOCP packet, later producers merge without
  allocation, and the owner clears the bit at packet dequeue without
  truncating the surrounding real-I/O batch. Deterministic before/after-reset
  races, multi-producer burst, self-rearm, quit, and closed-admission cases are
  covered by
  `contract.event_loop.test_event_loop_wakeup_coalescing`; its focused repeat
  passed 50/50, the complete Windows MSVC Debug inventory passed 111/111, and
  all 34 repository guards passed. These are local development-gate results,
  not frozen Release, sanitizer, endurance, or performance evidence.
- M3-G3 replaces the Windows full-output `Buffer` plus `writeStorage_` mirror
  with an owner-loop deque of stable string segments. `WSASend` borrows only
  the current front suffix, every submission is explicitly capped to `ULONG`,
  and partial completion advances an offset without moving the remaining
  bytes. The two new segmented/partial-write contracts passed 20/20 focused
  repeats each, the complete Windows MSVC Debug inventory passed 113/113, and
  all 31 repository/API guards passed. A same-runner, same-compiler Release
  comparison used seven alternating slow-client runs with four connections and
  8 MiB per connection: external peak-working-set median fell from
  113,942,528 bytes at baseline `61076a0` to 46,792,704 bytes in the current
  worktree, a 67,149,824-byte (58.93%) reduction. The benchmark's post-drain
  working-set-delta median fell from 67,481,600 to 352,256 bytes. This is local
  slice evidence, not a frozen M3 Release performance run.
- M3-G4 replaces the embedded 64 KiB Windows read array with one
  connection-local allocation created on the first `WSARecv`, reused behind a
  fixed 4 KiB submission/retention ceiling, and released only after final
  completion drain. It deliberately introduces no shared pool or cross-loop
  return path. On the same Windows runner and compiler, the structured Release
  10k-idle profile reduced working-set delta from 713,674,752 bytes
  (71,367.475 bytes/connection) at baseline `649880d` to 100,343,808 bytes
  (10,034.381 bytes/connection), an 85.94% reduction. Seven one-connection
  samples reduced the median delta from 126,976 to 61,440 bytes.
- M3-G5 replaces the single pending Windows `AcceptEx` with a configurable
  fixed pool that defaults to four and validates a hard maximum of 64. Every
  base-loop-owned slot has independent socket, `OVERLAPPED`, address storage,
  and generation; that checkpoint's Poller publication coalesced exact Accept
  operation identities through an allocation-free, operation-embedded Channel
  queue, which IOE-C1 later removed.
  Read/write same-Channel deferral was still present at that checkpoint and was
  later retired by the IOE-C1 operation-model slice. Retry cancels and
  consumes the complete submitted generation before
  replenishment, while Stop revokes every observer and final-drains one real
  obligation per submitted slot. The dedicated pool contract covers a 48-
  connection burst, callback `stop()` re-entry, deterministic synchronous
  failure after two real submissions, delayed Retry cancellation, fixed-depth
  accounting, and final zero slots/sockets/leases/obligations. On the same
  Windows runner and compiler, nine alternating Release samples of the
  128-connection preloaded accept-drain profile reduced median elapsed time
  from 1.680 ms at depth one to 1.524 ms at depth four, a 9.29% reduction
  (accept throughput +10.24%).
- M3-H1-A adds validated per-iteration budgets for active Channel dispatch,
  expired timers, and registered control callbacks, complementing the existing
  lifecycle and pending-functor budgets. Partial active batches remain
  EventLoop-owned and preserve O(1) remove-before-destroy invalidation between
  rounds; unselected timers/control bits remain in their source-owned ordered
  container/mailbox. Append-only metrics expose drained, exact remaining,
  oldest-ready lag, and budget exhaustion for every owner-loop callback phase.
- M3-H1-B makes the IOCP dequeue width startup-configurable in `[1, 64]`
  without changing the fixed 64-entry Poller storage ceiling. Configured-width
  tests preserve wakeup and exact Accept/read/write completion identity across
  later zero-timeout rounds, while append-only metrics distinguish drained
  packets, exact user-space deferred work, and full-width exhaustion without
  inventing an unavailable kernel queue depth or oldest-packet lag.
- M3-H1-C keeps active I/O, zero-delay timer, self-rearmed control/lifecycle,
  and self-queued functor work continuously ready for 12 deterministic owner
  rounds. Every source advances exactly once per round in the documented
  I/O -> timer -> control -> lifecycle -> functor order with callback-depth
  peak one.
- `connection_backpressure_controller` and `graceful_shutdown` are active
  `GameNet::core` implementation authority rather than deferred design assets.
- MetricsExporter is active but provisional, with thread-safe
  in-memory aggregation, immutable static labels, deterministic Prometheus
  snapshots, exception-contained Core/Logic/Broadcast recorder adapters, and
  bounded-cardinality metric names. Its current string/hash/global-mutex hot
  path is a reference implementation, not a production performance claim.
- The Release performance gate builds the reviewed baseline and candidate on
  the same runner, retains 72 raw samples per platform, compares medians for
  12 fixed Core/Phase-4 scenarios, and rejects parameter, hash, baseline, or
  budget drift. Candidate Core v2 output also has an independent semantic
  validator and a forced overload case, while historical Core v1 data remains
  performance-only. Cross-platform run `29808395220` passed at `5f926f3`; a
  later frozen runtime commit requires its own retained run.
- `integration.resilience.test_fault_injection` executes five production fault
  profiles in one cycle and is part of the ordinary Linux/Windows CTest matrix.
  The same executable can remain alive under a heartbeat supervisor for fixed
  24/72-hour Linux/epoll modes. Snapshot `b344318` completed both real modes
  with same-SHA pairing; shortened smoke evidence remains non-releasable and
  the final changed runtime must be requalified after freeze.
- At historical snapshot `b344318`, Windows MSVC Debug/Release preflights pass 89/89 configured
  CTests in 39.40/38.00 seconds. The Release fault-injection target separately
  passes 10/10 repeats in 1.98 seconds, the exact inventory verifier reports
  `threading=64`, `fault_injection=1`, and `endurance=1`, and all 30 Python
  guards pass.
- `tests/contract/socket/test_socket_contract.cpp` carries the Linux child-
  process/default-SIGPIPE contract.
- `tests/contract/tcp_connection/test_tcp_connection_high_water_mark.cpp`
  verifies hard-limit rejection plus read pause/resume hysteresis, and
  `test_tcp_connection_cross_thread_send.cpp` verifies pre-owner-loop byte
  reservation and overload rejection.
- `tests/contract/buffer/test_buffer_contract.cpp` verifies exact per-read caps;
  `test_tcp_connection_send_after_close.cpp` verifies unconsumed input reaches
  its configured cap and closes without further Buffer growth.
- `tests/contract/event_loop/test_event_loop.cpp` verifies normal-capacity
  rejection, explicit reserve exhaustion, accepted-work preservation, and a
  timer interleaving between bounded functor batches.
- `tests/contract/tcp_server/test_tcp_server_stop_active_write.cpp` verifies
  natural output drain, repeated shared completion, timeout force-close with
  exact counts, and immediate-stop compatibility.
- `tests/contract/socket/test_socket_contract.cpp` verifies invalid socket
  creation/bind/listen remain non-fatal, and
  `tests/contract/acceptor/test_acceptor_contract.cpp` verifies unavailable
  listener bind is reported by exception while the normal path leaves the
  error policy untouched.
- `tests/contract/event_loop/test_event_loop.cpp` verifies Channel, Timer,
  pending-functor, and metric exception sources, Continue/Quit behavior,
  throwing-policy containment, thread-init propagation, and partial worker-pool
  startup rollback.
- `tests/contract/tcp_server/test_tcp_server_contract.cpp` verifies message and
  disconnect callback exceptions close only the offending connection, observer
  exceptions remain contained, cleanup reaches zero connections, and a later
  client is still served. `tests/contract/connector/test_connector_contract.cpp`
  verifies a throwing diagnostic observer cannot interrupt connect success.
- The same TcpServer contract uses real clients to verify global and per-peer
  limits, bounded fixed-window rate rejection, cross-worker authentication,
  authentication timer cancellation, unauthenticated close, exact counters,
  base-loop metric affinity, and metric-callback exception containment.
- Windows MSVC Debug passes all 85/85 configured tests in 40.69 seconds after
  the recoverable socket/accept, callback-containment, and TcpServer admission
  changes; the focused admission contract also passes 20 consecutive runs.
- Production-hardening worktree Windows MSVC Debug AddressSanitizer passes
  85/85 in 47.38 seconds. CTest now supplies the selected compiler's ASan
  runtime directory per test, and the generated executable imports
  `clang_rt.asan_dynamic-x86_64.dll` without a caller-side `PATH` mutation.
- A clean Windows MSVC Release tree passes 85/85 in 38.45 seconds; its installed
  `GameNetCore 0.2.0` package configures, builds, and runs the external
  `find_package` consumer 1/1.
- The production-hardening worktree repeat-50 gates pass 3,050/3,050 threading
  executions in 1,716.27 seconds and 400/400 Pipeline/Broadcast executions in
  37.88 seconds. Both repository evidence verifiers accept the exact inventory,
  labels, repeat count, timeout, command, and result logs.
- All seven fixed Windows Release IOCP core/Phase-4 benchmark scenarios return
  their expected schema, platform/backend/build identity, and `status: ok`.
- Historical frozen production-hardening candidate
  `be749adc4bce7e1771b84c77c42bf080625805e9` is pushed on
  `codex/production-hardening`. Workflow-dispatch `ci` run `29791363106`
  passed all six producers and the aggregate evidence gate: Linux CMake,
  ASan/UBSan with 1,000 libFuzzer executions, TSan over all 61 threading
  tests, Linux Release, Windows Debug IOCP, and Windows Release IOCP. Each
  full-test producer executed 85/85 tests, and Linux plus both Windows package
  consumers executed 1/1.
- The same candidate owns successful `long-soak` run `29791364648`: repository
  verifiers independently accept 3,050/3,050 threading executions in 1,690.82
  seconds and 400/400 Pipeline/Broadcast executions in 34.19 seconds, with
  `repeat=50` and per-test `timeout=60`.
- The same candidate owns successful `core-benchmark` run `29791365828`.
  All eight fixed Core Release scenarios report `status: ok`, and the paired
  Phase 4 evidence gate verifies the three fixed scenarios on both Linux/epoll
  and Windows/IOCP with identical parameters. All remote sanitizer, package,
  long-soak, and benchmark evidence is bound to one frozen commit; Windows
  results are not used as a substitute for Linux execution.

## Verification State

The `6b29215` implementation checkpoint has 121 configured CTest tests: 8 unit
tests, 100 contract tests, and 13 integration tests. Its labels include 94
threading, 99 lifecycle, 7 game-pipeline, and 5 broadcast tests. The new direct
socket-option contract and capacity/performance/API repository guards pass on
Windows and WSL Linux; the new reviewed-surface diff is strictly empty. Full
local clean Debug/Release and remote same-SHA qualification belong to the new
candidate tag and are not inferred from these implementation-tree checks.

Phase 4 coverage includes bounded
PacketFramer/real-fuzz contracts, transport/session/logic lifecycle and race
contracts, seven Pipeline integrations, and five Broadcast
contracts/integrations. The current-roadmap additions cover the EventLoop
lifecycle hub, IOCP quit/completion drain, TcpConnection explicit close and
structured reasons, TcpServer aggregate stop/release handshake, and
TcpClientControl lifetime/close-reason propagation. Existing roadmap coverage
also includes EventLoop
control-lane saturation, optional TcpConnection notification saturation,
Connector/TcpClient typed admission and generation races, and IOCP synchronous
submission errors.

The remaining entries in this section are dated historical verification
records. Their counts and SHAs describe the named checkpoint, not the current
inventory or a final v0.3 candidate.

On 2026-07-29, the current Windows MSVC worktree passed a full Debug build and
114/114 CTests; the IOCP segmented/partial-write contracts passed 20/20 focused
repeats each and the bounded read-storage contract passed 50/50. The 2026-07-27 snapshot separately passed full
Debug, Release, and AddressSanitizer builds and 109/109 CTests in all three
configurations.
The M3-G5 worktree subsequently passed 115/115 Debug CTests, all 31 Python
repository/API/CI guards, and 50/50 focused `AcceptEx` pool repetitions.
The M3-H1-A worktree passed 116/116 Debug CTests, all 31 Python guards, and
50/50 focused fair-budget repetitions.
The completed M3-H1 worktree retained 116/116 Debug CTests and 31/31 guards;
configured-width Poller, Accept pool, and sustained-source fairness contracts
each passed 50/50 focused repetitions. Count budgets were sufficient to prove
bounded multi-source progress, so no time budget was introduced in this slice.
The M3-H2-A worktree preserves the legacy fixed-delay overload and adds an
explicit fixed-rate option with a finite consecutive catch-up cap followed by
cadence skip. The timer contract passed 20/20 focused repetitions, the full
Windows Debug inventory passed 116/116, and all 33 locally runnable
repository/API/CI checks passed (32 Python guards plus the structured API
diff; the migration-provenance verifier remains bound to CI's separately
checked-out source repository).
M3-H2-B adds round-robin, least-connections, queue-lag, and stable rendezvous
consistent-hash selection. Policy and exact connection-load accounting remain
base-loop-owned; queue-lag reports only synchronized pending-functor age/count,
and TcpServer uses normalized peer address as the consistent-hash key. The
selector contract passed 50/50 focused repetitions, the combined
TcpServer/EventLoopThreadPool lifecycle slice passed 12/12, the full Windows
Debug inventory passed 116/116, and all 33 locally runnable repository/API/CI
checks passed.
M3-H2-C adds one callback-free, owner-loop DeadlineQueue with upward bucket
quantization, generation-safe replacement/cancel, and a finite per-advance
budget. TcpServer authentication deadlines now share one driver and bounded
continuations; SessionManager idle deadlines share the same index, and the
Pipeline continues ready batches without rescanning its connection map. The
three focused DeadlineQueue/TcpServer/SessionManager contracts each passed
20/20 repetitions, the complete Windows Debug inventory passed 117/117, and
all 31 Python repository guards plus the public-manifest verifier and
structured API diff passed. Same-runner MSVC Release medians versus baseline
`437f294` reduced no-ready session expiry from 0.102/5.446/71.340 ms to
0.0003/0.0003/0.0004 ms at 10k/100k/1M sessions. Full-expiration cleanup
medians were 2.949/58.419/780.003 ms before and
3.9042/64.9958/740.6443 ms after; the bucket index intentionally optimizes
future-population isolation while keeping cleanup bounded, not every small-
population expire-all case.
M3-H2-D does not introduce per-worker listeners. Seven Windows MSVC Release
preloaded 128-connection samples produced 1.597/1.586/1.567 ms median
ready-backlog drain for 1/2/4 workers. Five successful live 1,024-connection
samples per worker count produced 1.519/2.014/2.016 s medians, with one retry
needed for the one-worker set. The isolated base-loop backlog was already
roughly three orders of magnitude faster than the live handshake interval, so
this runner does not prove a base-loop accept bottleneck. Linux/epoll
`SO_REUSEPORT` per-worker accept remains deferred until M3-P1 has a sustained
churn profile that isolates and saturates ready accept work.
M3-Q1-A adds mutex-free hierarchical TCP output admission in the fixed order
connection -> owner loop -> server -> optional caller-shared global budget.
Every shared scope has a hard limit, lower recovery threshold, atomic
current/peak/rejection/overload snapshot, and exact rollback/release. The
connection keeps its existing hard limit and now exposes the same diagnostic
snapshot; `TcpSendResult` identifies the first rejecting scope. TcpServer
defaults to finite 64 MiB per-loop and 256 MiB per-server budgets and installs
immutable budget identities before connection establishment. The two new
budget/server contracts each passed 50/50 focused repetitions, including a
32-thread no-overshoot race, two-worker scope injection, later-scope rollback,
and zero pending bytes after stop. The Windows partial-write contract was also
made independent of ambient system timer-resolution changes while preserving a
reader slower than each physical write chunk.
M3-Q1-B replaces BroadcastDispatcher's per-task global accounting mutex with
an immutable, atomically published owner registry and the fixed reservation
order owner task -> owner logical bytes -> global logical bytes. First-owner
registration alone uses a mutex; repeated admission, later-scope rollback,
queue rollback, completion release, shutdown, and snapshots do not. The
provisional diagnostics now expose global and per-owner current/peak/rejection
state while preserving aggregate `tasks == 0` as the exact convergence marker.
The outstanding-budget contract passed 100/100 focused repetitions with 32
concurrent producers at owner/global limits and a concurrent shutdown race;
all five Broadcast contracts then passed 20 consecutive repetitions. The
resulting MSVC Debug tree passed all 119 tests and all 33 repository guards;
the public API manifest and deterministic compatibility diff also passed.
M3-Q1-C adds hysteretic retained-capacity governance to Core Buffer and the
provisional PacketFramer. Buffer defaults to a 64 KiB-plus-prepend retention
target with a 16 KiB readable recovery threshold; PacketFramer defaults to one
maximum-size frame for both target and threshold. Active data may exceed the
target, but low-water trim preserves unread/ring order, records current/peak/
trim state, and keeps the old valid allocation if replacement allocation
fails. Buffer and PacketFramer direct contracts each passed 50/50 repetitions;
the Buffer unit plus all three PacketFramer contracts passed together, and the
MSVC Debug tree passed all 119 tests. All 33 repository guards plus the public
API manifest and deterministic compatibility diff also passed.
M3-Q1-D inventories transport-internal pool, slab, and fixed working storage
outside logical pending-byte budgets. The stable process snapshot records
AcceptEx fixed-pool bytes through final shared completion-lease release, the
Poller-lifetime IOCP completion workspace, and optional connection-local 4 KiB
read chunks; the active architecture explicitly reports shared read-pool and
shared-slab bytes as zero. Category and aggregate current/peak counters update
only on construction/allocation and release/destruction boundaries. The three
direct contracts each passed 50/50 repetitions, including Acceptor stop before
final lease release and a cross-thread snapshot observation. The resulting
MSVC Debug tree passed all 119 tests; all 31 Python repository/API contracts,
the public API verifier, and deterministic compatibility diff also passed.
M3-Q1-E adds an opt-in real-TCP
`gamenet_capacity_profile --scenario slow-broadcast-recovery` and its strict
`gamenet.capacity_profile.v1` validator. The first Windows MSVC Release seed
held four 4 KiB-window clients unread while dispatching 64 shared 256 KiB
payloads across two worker loops. Of 256 endpoint attempts, 40 were accepted
and 216 were rejected as `EndpointOverloaded`; the latter reconciled exactly
with 216 connection-scope TCP reservations and zero loop/server/global
rejections. Aggregate pending peaked at its exact 8 MiB hard-limit sum,
Broadcast outstanding peaked at 15,728,640 of 67,108,864 bytes, and recovery
reached zero pending for a 250 ms stable window in 276.303 ms. Recovered
Buffer capacity was 8,256 bytes, connection-local read storage was the exact
4 x 4 KiB limit, and every process fixed-storage current counter reached zero
after teardown. The real Broadcast integration recovery path passed 50
consecutive Release repetitions. RSS remains an observational capacity
measurement rather than a correctness threshold; larger 100/1k slow-reader
and 1k/10k+ Broadcast runs remain M3-P1 candidate work.
The current inventory is unit=8, contract=99, integration=13, threading=93,
lifecycle=98, game_pipeline=7, and broadcast=5. All 31 Python repository/API
contracts passed before M3-Q1-A. After Q1-A, all 119 MSVC Debug tests and all
33 repository guards passed; the public API manifest and deterministic
compatibility diff also passed. The focused 12-test Pipeline/Broadcast slice
passed repeat 50 for 600 executions. A clean Release install exposed
`DispatchResult` and the updated upper-layer headers, and the isolated
`find_package(GameNetCore)` consumer built and passed CTest 1/1.

The pre-H2-C Release `session-expiry-scan` scale study measured
0.10/5.95/70.01 ms for 10k/100k/1M active sessions; full scan and cleanup
measured 3.17/59.63/797.36 ms. Those historical samples and the H2-C comparison
above are local trends rather than reviewed regression thresholds. Current
runtime changes still require a frozen candidate SHA plus Linux Debug/Release,
Linux ASan/UBSan, Linux TSan, cross-platform performance, and 24/72-hour
endurance evidence before any stable promotion.

Local Phase 4 hardening `final-v4` preflight subsequently frozen into candidate
`5ebad2c1a4a9487437340935e21f7468140c7e8d`:

- Windows MSVC Debug: 85/85 configured tests passed in 36.47 seconds.
- Windows MSVC Release: 85/85 configured tests passed in 36.97 seconds.
- Linux Clang 19 Release: 85/85 configured tests passed in 34.76 seconds.
- All 27 Python scope, intent, CI, and CMake guards passed locally.
- PacketFramer contracts and deterministic round-trip harness compile under GNU
  g++ C++23 with `-Wall -Wextra -Wpedantic -Werror`. The real libFuzzer target
  also completed exactly 1000 local Clang 19 ASan/UBSan runs with 6 binary
  seeds, its dictionary, fixed seed `424242`, `max_len=8192`, and a 2-second
  input timeout. The command did not use `max_total_time`; the saved mutated
  corpus contains 90 files and no crash artifact. Candidate main-CI run
  `29160903594` repeated the sanitizer-backed fuzz gate successfully and
  retained its SHA-bound log, corpus, dictionary, and artifact evidence.
- The current Debug inventory includes 82 threading-labeled tests, 88
  lifecycle-labeled tests, and twelve Pipeline/Broadcast tests. These current
  runtime changes still require new repeat and remote evidence. On the
  historical `final-v4` tree, its 61-test threading selection passed repeat 50:
  61 x 50 = 3,050 executions with zero failures in 1,777.76 seconds. The
  focused Pipeline/Broadcast slice also passed repeat 50: 8 x 50 =
  400 executions with zero failures in 54.16 seconds. Both
  `gamenet.ctest_repeat_evidence.v1` manifests report `result: success` and bind
  to the same `final-v4` inventory SHA-256
  `37ee7fb3572c911fa771ba42ce1fcb91a252bc2c78c56b98b280f5305c77a09a`.
  Candidate `long-soak` run `29161167423` then completed the same exact
  selections remotely: 3,050/3,050 threading and 400/400 Pipeline/Broadcast
  executions, with both structured verifiers, the job manifest, and artifact
  upload successful.
- Release install/export for package version `0.2.0` installs
  `GameNet::core`, `GameNet::protocol`, `GameNet::transport`,
  `GameNet::game_session`, `GameNet::game_logic`, and `GameNet::broadcast`.
  Clean Linux Clang and Windows MSVC Release external consumers discovered the
  exact version with `find_package(GameNetCore 0.2.0 EXACT)`, linked all six
  targets, exercised transport/session API, and each passed executable CTest
  1/1. In `final-v4`, the Linux consumer completed 1/1 in 0.02 seconds and the
  Windows consumer reported a 0.02-second test case (0.03 seconds total CTest
  time).
- A full Windows MSVC Debug AddressSanitizer checkpoint passed all 85/85 CTests
  in 43.19 seconds.
  Its first full run exposed a Connector ConnectEx timeout/cancel late-
  completion Channel use-after-free; Poller-retained operation state and
  completion-drain lifetime fixed it before the green rerun.
- Linux Clang 19 ASan/UBSan also passed the full current 85/85 CTest inventory
  in 36.18 seconds in the Docker validation environment.
- Linux Clang 19 TSan passed the complete 61/61 `threading` inventory after a
  genuine test-fixture race was fixed: the repeating TimerQueue cancellation
  handle is now initialized, read, and canceled on its owner loop. Docker's
  default seccomp had to be relaxed for TSan's `ADDR_NO_RANDOMIZE` personality
  syscall; the container was not run privileged. The `final-v4` threading run
  completed in 35.63 seconds.
- The `final-v4` Linux Clang/epoll and Windows MSVC/IOCP Release Phase 4 benchmark
  executables each report `status: ok` for framing, logic-queue, and broadcast-
  fanout. Both raw three-file sets pass the shared semantic/count-invariant
  validator. They are local snapshots from different execution environments,
  not performance thresholds or direct cross-host scores. Candidate benchmark
  run `29161168417` subsequently completed both remote producers and the
  aggregation-only `gamenet.phase4_benchmark_pair_evidence.v1` gate successfully.
  Linux framing/logic-queue/broadcast-fanout recorded 9444.046 MiB/s,
  452934.439 ops/s, and 5036891.294 ops/s; Windows recorded 2575.474 MiB/s,
  32475.926 ops/s, and 4535488.872 ops/s respectively.
- Pipeline lifecycle tests now prove that a strong callback-state reference does
  not preserve execution permission after atomic revocation. They cover
  synchronous `stop()` from a Logic-stage callback when management and logic
  share one loop, an endpoint observer spanning revocation with no later send,
  and one exact frame batch containing authentication plus an early command.
  Session lifecycle coverage keeps live heartbeat and offline producers
  overlapping the management-loop drain and uses per-producer sentinels to prove
  final Offline state and empty indexes only after all posts drain.
- At the frozen Phase 4 candidate checkpoint, intent semantics resolved all 25
  active targets and 74 explicit verification paths. The 16 frozen Core
  library intents retained only their
  documented metadata exemption; seven enriched Phase 4 intents still require
  artifact kind, provenance, and non-empty verification. The dependency guard
  derives the six production library edges from an actual configured CMake
  target graph and rejects direct and transitive reverse dependencies through
  negative fixtures.
- Clean-checkout CI no longer relies on the ignored local `mini_trantor/` tree:
  all semantic-guard jobs checkout `YanqingXu/mini_trantor` at immutable commit
  `3eba368475a68f677aae920d4f299b155db23d57`, then verify 20 declared source
  paths directly against that Git object before intent semantics run.
- The repository now configures six CI producer jobs: Linux Debug, Linux
  ASan/UBSan plus real fuzz, Linux TSan, Linux Release, Windows Debug IOCP, and
  Windows Release. Each producer locks the 85-test inventory (TSan also locks
  `threading=61`), executed consumers lock one test, and 90-day evidence bundles
  contain JUnit/raw logs plus a SHA/run/attempt-bound `gamenet.ci_evidence.v1`
  manifest; ASan also preserves the fuzz dictionary, mutated corpus, log, and
  artifacts. A seventh aggregation-only gate validates exact job identities,
  selected/executed tests, shared candidate/run identity, bytes, and hashes, then
  emits `gamenet.ci_evidence_set.v1`. It is not a seventh platform producer.
  The manual long-soak workflow emits `gamenet.ctest_repeat_evidence.v1` for the
  exact selected tests and repeat count, while the two Phase 4 benchmark
  producers feed an aggregation-only pair gate that emits
  `gamenet.phase4_benchmark_pair_evidence.v1` for one Linux/epoll and one
  Windows/IOCP result with identical scenario parameters.
  Functional candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d` is committed and
  pushed, and was the PR #4 head when candidate evidence was produced.
  Pull-request `ci` run
  `29160903594` checked GitHub merge-ref
  `e461b597f2642e000717f536f3b430b804ba26ad`, bound candidate and PR-head
  identity to `5ebad2c1a4a9487437340935e21f7468140c7e8d`, and passed all six
  producers plus the aggregate gate, 7/7. The same candidate owns successful
  `long-soak` run `29161167423` and paired benchmark run `29161168417`.
  Final PR-head run `29162961320` passed 7/7 before PR #4 was owner-authorized
  and merged with merge commit `7668d6b82a0d815ccd79f83c572bc0a36bcceea0`.
  Main push run `29168786199` validated that exact commit with six producers
  plus aggregate, 7/7. Annotated tag `v0.2.0-phase4-preview` (tag object
  `b76077f839230fb99f5e570ef623174747f04249`) and the
  [formal GitHub prerelease](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview)
  are published. PR #4 had zero submitted GitHub reviews; that process
  limitation is retained rather than described as completed review.

Pre-hardening Phase 4 baseline retained as immutable historical evidence:

- Windows MSVC Debug: 74/74 passed in 34.67 seconds.
- Windows MSVC Release: 74/74 passed in 34.41 seconds.
- PacketFramer contract and deterministic round-trip smoke also compiled and ran with GNU g++ C++23
  under `-Wall -Wextra -Wpedantic -Werror`.
- Windows MSVC Debug threading slice: 51/51 passed across repeat-5 (255 test
  executions) in 170.00 seconds with a 30-second per-test timeout.
- The six exported targets were installed, discovered, linked, and executed by
  the downstream install consumer.
- Pre-hardening PR #4 head `0d62054e148a1c95793799eb88856363ac6843d3`
  was associated with five-job `ci` run `29147391402` (#32) on 2026-07-11;
  that run actually checked out and tested GitHub merge-ref
  `31107e8964a0f206087fe2f029a39a15107f6bda`, not the head object itself.
- The TSan job in that historical run selected the then-current `threading`
  label inventory and passed 51/51 tests. That count belongs to the old
  74-test tree; it is not the current 61-test threading inventory.
- That historical run predates the current remediation and did not include the
  sixth Windows Release job, real libFuzzer execution, repeat-50 Phase 4 soak,
  Phase 4 benchmark artifacts, or formal preview-release evidence. It cannot
  certify candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d`. The immutable
  Phase 3.5 Core Preview evidence remains recorded separately below.

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON`
- Build: `cmake --build build --parallel`
- Test: `ctest --test-dir build --output-on-failure`
- Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`
  (this fixed label belongs to the Phase 3.5 release record; the verified
  Phase 4 functional candidate and runs are recorded above).
- CI workflow run id: `29079836593` (`ci` #29, `main`).
- Validation date: 2026-07-10.
- Result: all five jobs passed:
  `Linux CMake build and tests`, `Linux ASan/UBSan build and tests`,
  `Linux TSan race-oriented build and tests`, `Linux Release build`, and
  `Windows MSVC IOCP build and tests`.
- Release: annotated tag `v0.1.0-core-preview` peels to the validated commit.
  Its tag-only release record and known limitations are documented in
  `docs/development/releases/v0.1.0-core-preview.md`; no GitHub Release assets
  or historical checksum manifest existed.
- Focused candidate commit `a7fd77cbd2140041cebb3f900d5c609fafc2adad`
  passed PR `ci` run `29076601085` (#27) and is preserved as a parent of the
  release merge commit. It owns the same-SHA long-soak and benchmark evidence
  recorded below; merge commit `c4818d4` combines it with evidence documentation
  without rewriting either commit.
- The preceding audited candidate, commit
  `d1474b5f32e609a7d2e2648af31b45635595d304`, failed in `ci` run id
  `29073362905` (#26) on
  `contract.tcp_client.test_tcp_client_repeated_connect` at
  `serverConnectedCount == 1`. Linux synchronously completed the first
  connection teardown before queued duplicate `connect()` submissions were
  drained, allowing a second Connector attempt. The validated fix
  coalesces one generation-tagged connect request across each pending/active
  lifecycle and releases it on terminal no-retry failure or connection
  removal.
- Race-oriented CI: this worktree adds a `linux-tsan` workflow job named
  `Linux TSan race-oriented build and tests`. It configures with
  `GAMENET_ENABLE_TSAN=ON`, builds the Debug target set, and runs the CTest
  suite filtered to the `threading` label. That label includes cross-thread
  APIs, worker-loop scheduling, pending read/write forceClose cancel-close
  contracts, direct Connector retry-stop cancellation contracts,
  mixed-timing pending ConnectEx stop contracts,
  active retry-enabled TcpClient stop-after-peer-close contracts,
  active cross-thread TcpClient disconnect contracts,
  repeated active TcpClient disconnect idempotence contracts,
  repeated active TcpClient stop idempotence contracts,
  repeated active TcpClient connect idempotence contracts,
  active cross-thread TcpClient connect contracts,
  active cross-thread TcpClient retry configuration contracts,
  post-close TcpConnection send ignore contracts, mixed-timing pending-read forceClose contracts,
  mixed-timing pending-write forceClose contracts,
  repeated TcpConnection shutdown idempotence contracts,
  worker-owned active-write stop contracts, worker-callback TcpServer stop
  contracts, and repeated TcpServer stop idempotence contracts.
  Phase 4 candidate race-oriented remote evidence is the successful Linux TSan
  producer in `ci` run `29160903594`. Latest recorded race-oriented CI remote green evidence is `ci` #29 on release commit
  `c4818d4b3956c85830e04d4a1f32df4ad701d453` only within the preserved Phase
  3.5 Core history; it is not the current Phase 4 evidence.
- Scope guard: local self-test and repository scan pass; CI runs both before
  CMake configure.
- Intent/documentation guards: CI runs the intent consistency guard, intent metadata contract guard, Core benchmark contract guard, Runtime Profile contract guard, Logger thread-contract guard, EventLoop contract guard, TCP lifecycle contract guard, TcpConnection context contract guard, TcpConnection thread-contract guard, EventLoopThreadPool contract guard, TimerQueue contract guard, threading gate contract guard, migration status contract guard, install/package contract guard, MSVC UTF-8 build contract guard, platform backend contract guard, Windows IOCP milestone contract guard, Windows IOCP data-path contract guard, sanitizer flag contract guard, Release-safe test guard, and workflow job structure guard before CMake configure. The EventLoop contract guard now also requires the cross-thread-observed pending functor execution state to be atomic or synchronized.
- Historical intent-governance snapshot (2026-07-06, not the current inventory):
  all 60 formal `*.intent.md` documents at that checkpoint carried ordered
  `status`, `target`, `migration_source`, and `promote_gate` front matter and
  appear exactly once in the intent index: 25 active contracts, 23 deferred design assets, and 11 legacy source-project stage documents.
  Active bodies reject stale `MINI_ENABLE_*`, `mini::`, `mini/net`, and
  `mini-trantor` contracts. Deferred bodies require an explicit future gate;
  legacy v1/v2/v3/v5/v6 and M1-M32 documents target `historical` with
  `promote_gate: never`, so their old options, test counts, and phase claims are
  not current repository evidence. At that historical checkpoint, the semantic
  guard dynamically resolved all 25 active targets and their 74 explicit
  verification paths: 69 regular C++
  CTest sources, one libFuzzer target, and four Python governance tests executed
  by the workflow. Six new active Phase 4 component/use-case intents target
  protocol, transport, game_session, game_logic, broadcast, and the pipeline
  example; a seventh active Phase 4 intent defines the opt-in performance
  baseline, and the echo use case remains an active example contract. The 16
  frozen Core library intents must still resolve to the real installed
  `GameNet::core` target but retain the documented artifact-kind, provenance,
  and non-empty-verification metadata exemption. Production dependency
  direction is checked from a configured CMake target graph, including
  transitive reachability and direct/transitive reverse-dependency fixtures.
- Added lifecycle and base coverage in this worktree: coost-compatible Logger unit and contract coverage, concurrent Logger runtime-configuration coverage (`contract.base.test_logger_thread_safety`), EventLoop cross-thread pending-functor execution-state atomicity guard, cross-thread TcpConnection state observation (`contract.tcp_connection.test_tcp_connection_cross_thread_state`), Connector completed-Channel member release guarded by the repeated-connect contract, server stop with active connections, server stop during active write, server stop soak for worker-owned connections, server multi-worker stop from the base loop, server worker-owned active-write stop, server worker-callback TcpServer stop soak, server repeated stop idempotence (`contract.tcp_server.test_tcp_server_repeated_stop`), client retry stop race, client retry-stop soak, direct Connector retry-stop cancellation (`contract.connector.test_connector_retry_stop`), client stop during pending ConnectEx, client pending ConnectEx stop soak, client cross-thread stop during pending ConnectEx, client mixed-timing pending ConnectEx stop soak, client destruction during pending ConnectEx, client destruction with active TcpConnection, client mixed-timing active-connection stop soak, client cross-thread active disconnect, client repeated active disconnect idempotence, client repeated active stop idempotence (`contract.tcp_client.test_tcp_client_repeated_stop`), client repeated active connect idempotence (`contract.tcp_client.test_tcp_client_repeated_connect`), client cross-thread active connect, client cross-thread retry configuration (`contract.tcp_client.test_tcp_client_cross_thread_retry_config`), peer close convergence, peer reset convergence, error-triggered teardown idempotence, cross-thread send delivery, post-close TcpConnection send ignore (`contract.tcp_connection.test_tcp_connection_send_after_close`), write-complete callback ordering, shutdown while output pending, cross-thread shutdown draining, repeated TcpConnection shutdown idempotence (`contract.tcp_connection.test_tcp_connection_repeated_shutdown`), high-water mark notification, repeated forceClose idempotence, repeated connectDestroyed stale-registration cleanup (`contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed`), cross-thread forceClose soak, cross-thread pending-read forceClose, cross-thread pending-write forceClose, pending-read forceClose cancellation before connection destruction, mixed-timing pending-read forceClose soak, pending-write forceClose soak before connection destruction, mixed-timing pending-write forceClose soak, TimerQueue ready-timer cancellation race coverage, legacy fixed-delay plus fixed-rate bounded catch-up/skip coverage, EventLoopThreadPool queued-work soak coverage, and EventLoopThreadPool restart-stop soak coverage.
- The repeated-connect contract now also covers generation-gated request
  admission and a fresh explicit connect after terminal no-retry failure.
- Test support hardening: repeated TcpConnection lifecycle/race setup now uses
  shared tests/support helpers. `SocketPair.h` centralizes socketpair,
  nonblocking, and small-send-buffer setup; `ClientSocket.h` centralizes nonblocking test-client connect and cleanup
  for TcpServer lifecycle contracts, Acceptor/Socket contracts, and
  TcpConnection peer-reset setup; `TcpConnectionHarness.h`
  centralizes loop-bound TcpConnection construction from connected socketpair
  fixtures; `TcpConnectionCallbacks.h`
  centralizes owner-loop connection/disconnection/close callback counting for
  force-close contracts; `LoopTest.h` centralizes EventLoop watchdog execution
  for TcpConnection force-close contracts, TcpClient lifecycle watchdogs, and
  TcpServer lifecycle watchdogs;
  `ThreadHandoff.h` centralizes one-shot and delayed non-owner-thread handoff
  for cross-thread lifecycle contracts; `FutureTest.h` centralizes bounded future waits for EventLoop, EventLoopThread, TimerQueue, and EventLoopThreadPool async contract tests; and
  `TcpClientStopHarness.h` centralizes retry-stop stale-reconnect assertions
  for client lifecycle contracts; `TcpServerHarness.h` centralizes multi-client TcpServer connection setup and worker-loop distribution assertions
  for server lifecycle/race contracts.
- Sanitizers: CI includes an ASan/UBSan Debug build and CTest job for the
  Reactor / TCP foundation. The worktree also defines a Linux TSan
  race-oriented threading test gate for thread-affinity and lifecycle risks.
- Long soak: this worktree adds a non-default `long-soak` workflow. It is
  manual-only through `workflow_dispatch` and runs the `threading` CTest slice
  with `ctest --repeat until-fail` so mixed-timing lifecycle contracts can
  gather stronger soak evidence without blocking ordinary push or pull-request
  CI. The long-soak repository guard parity includes the EventLoop contract guard,
  keeping manual soak guards aligned with the ordinary CI guard surface. The
  current workflow input defaults to repeat 50 with a 60-second per-test
  timeout. The workflow locks the 115-test inventory, `threading=88`,
  `game_pipeline=7`, and `broadcast=5`, then verifies the raw result lines for
  every selected test and exact repeat count. Its two
  `gamenet.ctest_repeat_evidence.v1` summaries include per-test executions,
  elapsed time, command, inventory hash, and raw-log hash; the enclosing
  canonical SHA/run/attempt artifact also carries `gamenet.ci_evidence.v1`.
  Local Windows Debug long-soak evidence also covers the previous
  43-test threading slice before the cross-thread TcpClient retry configuration
  contract expanded the threading label to 44 tests. That earlier local run used:
  `ctest --test-dir build -C Debug --output-on-failure -L threading --repeat until-fail:20 --timeout 60`;
  43/43 threading-labeled tests passed across 20 repeats on 2026-07-09,
  and CTest reported total test time was 637.56 seconds. The then-expanded
  44-test threading slice was covered once by the full Windows Debug and Release
  CTest checkpoint. The new cross-thread TcpConnection state contract expands the
  threading slice to 45 tests. The Logger concurrency contract expanded the
  then-current threading slice to 46 tests.
  The 2026-07-10 Windows Debug preflight passed all 46 threading tests across 5
  repeats with `--timeout 15`; CTest reported 176.90 seconds on 2026-07-10.
  The repaired repeated-connect contract separately passes 50 repeats in
  32.41 seconds. The corresponding historical remote GitHub `long-soak`
  evidence is recorded in run
  `29077148022`, job `86311227712`, commit
  `a7fd77cbd2140041cebb3f900d5c609fafc2adad`, repeat 50, timeout 60 seconds,
  completed successfully at 2026-07-10T08:04:12Z. The log records
  `ctest --test-dir build-long-soak --output-on-failure -L threading --repeat until-fail:50 --timeout 60`;
  46/46 threading-labeled tests passed in 1632.47 seconds of real CTest time,
  and the complete job took 28m27s. Historical run `28986707243` covered the
  earlier 36-test slice at repeat 20 on commit
  `9b27a0a3c3993cb1f90ef4357fa80027205ca221`.
- Release: CI includes a Release build and CTest gate for the Reactor / TCP
  foundation. C++ tests use a Release-safe helper instead of standard `assert`
  so contract checks remain active with `NDEBUG`. Local Windows Release
  evidence now also passes after shortening generated CMake test target names
  to keep MSBuild `.tlog` paths below Windows path-length limits:
  `cmake --build build-release --config Release --parallel` succeeds, and
  `ctest --test-dir build-release -C Release --output-on-failure --timeout 10`
  reports 67/67 Release tests passed for the then-current Core worktree in 37.38 seconds
  on 2026-07-10.
- Core benchmark: `GAMENET_BUILD_BENCHMARKS` defaults to `OFF`; when enabled it
  builds the non-installed, non-CTest `gamenet_core_benchmark` executable. Its
  `gamenet.core_benchmark.v1` JSON covers echo throughput/P50/P99, one-versus-two
  worker scaling, idle-connection process working set, and slow-client output
  accumulation while identifying backend and completion mode. Local Windows
  MSVC Release evidence on 2026-07-10 records 51.979 MiB/s and P99 61.7 us with
  one worker, 73.994 MiB/s and P99 59.2 us with two workers, about 71,264 bytes
  of process working-set delta per idle connection at 256 connections, and a
  67,465,216-byte working-set increase after offering 8 MiB to each of four
  non-reading clients. The slow-client run fired four high-water callbacks and
  explicitly reports `high_water_notification_only`; it is not a memory-cap
  claim. Raw local evidence is under
  `docs/development/benchmark_results/2026-07-10-windows-msvc-release/`.
  The manual-only `core-benchmark` workflow run `29077151229` completed successfully on
  `a7fd77cbd2140041cebb3f900d5c609fafc2adad` and published matching Linux
  epoll (`epoll_wait_batch`) and Windows IOCP
  (`single_get_queued_completion_status`) Release artifacts. Both artifact
  names include the full commit SHA and each contains the same four scenarios.
  Linux one/two-worker echo measured 32.337/65.234 MiB/s with P99
  97.419/74.229 us; Windows measured 16.188/26.110 MiB/s with P99
  162.8/151.3 us. The 256-connection working-set deltas were 991,232 bytes on
  Linux and 18,137,088 bytes on Windows. Four slow clients offered 8 MiB each,
  producing four high-water callbacks and working-set deltas of 26,562,560
  bytes on Linux and 67,493,888 bytes on Windows. All eight JSON files use
  `gamenet.core_benchmark.v1` and report `status: ok`; these values are evidence
  snapshots, not performance thresholds.
- Install/package: CI installs the core target and builds an external consumer
  fixture through `find_package(GameNetCore)` and `GameNet::core`. Release
  install/package consumer also passes locally: the Release build installs to
  `build-release/_install`, the external fixture configures into
  `build-release-install-consumer`, builds in Release, and the
  `gamenet_install_consumer` executable exits 0.
- Windows: Windows support is now represented by a `windows-msvc` workflow job
  for the Reactor / TCP IOCP backend. Local VS2026 Debug configure/build
  succeeds after the MSVC `/utf-8` and `/FS` compile options were added, and a
  local full Windows Debug CTest run with a 10-second per-test timeout passes
  67/67 configured tests with 0 failing tests in 43.57 seconds on 2026-07-10.
  The IOCP data path now covers
  `contract.event_loop.test_event_loop`,
  `unit.base.test_logger`,
  `contract.base.test_logger_contract`,
  `contract.base.test_logger_thread_safety`,
  `contract.event_loop_thread_pool.test_event_loop_thread_pool`,
  `contract.timer_queue.test_timer_queue`,
  `contract.acceptor.test_acceptor_contract`,
  `contract.connector.test_connector_contract`,
  `contract.connector.test_connector_retry_stop`,
  `contract.poller.test_poller_contract`,
  `contract.tcp_client.test_tcp_client_contract`,
  `contract.tcp_client.test_tcp_client_retry_stop_soak`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect_soak`,
  `contract.tcp_client.test_tcp_client_cross_thread_stop_pending_connect`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect_mixed_timing_soak`,
  `contract.tcp_client.test_tcp_client_destroy_pending_connect`,
  `contract.tcp_client.test_tcp_client_destroy_active_connection`,
  `contract.tcp_client.test_tcp_client_stop_active_connection_mixed_timing_soak`,
  `contract.tcp_client.test_tcp_client_cross_thread_disconnect_active`,
  `contract.tcp_client.test_tcp_client_repeated_disconnect`,
  `contract.tcp_client.test_tcp_client_repeated_stop`,
  `contract.tcp_client.test_tcp_client_repeated_connect`,
  `contract.tcp_client.test_tcp_client_cross_thread_connect`,
  `contract.tcp_client.test_tcp_client_cross_thread_retry_config`,
  `contract.tcp_server.test_tcp_server_contract`,
  `contract.tcp_server.test_tcp_server_stop_active_connections`,
  `contract.tcp_server.test_tcp_server_stop_active_write`,
  `contract.tcp_server.test_tcp_server_stop_multi_worker`,
  `contract.tcp_server.test_tcp_server_stop_worker_active_write_soak`,
  `contract.tcp_server.test_tcp_server_stop_from_worker_callback_soak`,
  `contract.tcp_server.test_tcp_server_repeated_stop`,
  `contract.tcp_server.test_tcp_server_stop_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_send`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_state`,
  `contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed`,
  `contract.tcp_connection.test_tcp_connection_send_after_close`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_write`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_shutdown`,
  `contract.tcp_connection.test_tcp_connection_repeated_shutdown`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_read`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_read_mixed_timing_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_mixed_timing_soak`,
  TcpConnection lifecycle/read/write/cancel-close contracts, and
  `integration.tcp.test_tcp_server_client_echo` through
  `PostQueuedCompletionStatus`, `AcceptEx`, `ConnectEx`, `WSARecv`, and
  `WSASend`. The active Windows source selection points at the IOCP backend,
  the legacy select backend files have been removed from the active target, and
  a select-based Windows CI job must not be promoted as the performance target.
  The Windows install/package consumer gate also passes locally: the VS2026
  Debug build installs to `build-local-vs2026/_install`, and the external
  `tests/cmake/install_consumer` fixture configures, builds, and runs through
  `find_package(GameNetCore)` and `GameNet::core`. The Windows workflow uses
  the Visual Studio generator, Debug CTest, install, and external package
  consumer gates. The latest recorded green Windows job is `ci` #29, run
  `29079836593`, on release commit `c4818d4b3956c85830e04d4a1f32df4ad701d453`;
  all four Linux jobs in the same workflow also passed.
  The IOCP data-path design and implementation plan are recorded in
  `docs/superpowers/specs/2026-07-07-windows-iocp-data-path-design.md` and
  `docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`. See
  `docs/development/windows_iocp_milestone.md`.

## Phase 4 Implementation State

The Phase 4 entry evidence gate was satisfied by the annotated preview tag
`v0.1.0-core-preview`; no GitHub Release was published for that tag. Candidate
`a7fd77cbd2140041cebb3f900d5c609fafc2adad`
owns the repeat-soak and benchmark artifacts, while release commit
`c4818d4b3956c85830e04d4a1f32df4ad701d453` owns the final main-branch CI and
tag:

- [x] Fresh candidate-SHA remote CI evidence is recorded for Linux CMake, Linux ASan/UBSan, Linux TSan, Linux Release, and Windows MSVC IOCP.
- [x] The remote `long-soak` workflow has a green run recorded with run id, commit sha, repeat count, timeout, date, result, and duration.
- [x] `docs/migration_status.md`, `docs/development/ci.md`, and `docs/development/windows_iocp_milestone.md` have no pending evidence for the validated candidate.
- [x] TcpConnection, TcpClient/Connector, and TcpServer lifecycle/race tests have no known flaky entries.
- [x] Linux and Windows install/package consumers pass through `find_package(GameNetCore)` and `GameNet::core`.
- [x] Matching Release `gamenet.core_benchmark.v1` evidence is recorded for Linux
  epoll and Windows IOCP with commands, scenario parameters, backend, and
  completion mode.
- [x] PR #2 is merged and annotated tag `v0.1.0-core-preview` points at the
  validated release commit; this was a tag-only preview, not a GitHub Release.
- [x] PacketFramer has an approved active intent, independent target, invariants,
  contracts, segmented-input coverage, and deterministic round-trip smoke.
- [x] TransportEndpoint/TCP adapter, SessionManager, bounded LogicLoop queue,
  pipeline example/integration, and BroadcastRouter/Dispatcher are implemented
  behind independent targets and contracts.
- [x] Core remains free of reverse dependencies; the scope guard enforces the
  allowed component matrix for core and every installed Phase 4 layer.
- [x] The pre-hardening local baseline recorded Windows Debug/Release pass
  74/74 and a downstream consumer linking all six exported targets; this line
  is historical and does not describe the current test inventory.
- [x] The `final-v4` local hardening preflight frozen into candidate `5ebad2c1`
  passes 85/85 in Windows Debug,
  Windows Release, and Linux Clang Release, passes all 27 Python guards, and
  passes executable Linux/Windows Release package consumers against exact
  version `0.2.0`.
- [x] Full Windows MSVC ASan and Linux Clang ASan/UBSan pass 85/85 after the
  IOCP lifetime repairs; Linux TSan passes 61/61 after the TimerQueue fixture
  repair; real libFuzzer completes 1000 runs; and both local platform sets of
  fixed Release Phase 4 benchmarks report `status: ok` and validate.
- [x] The audited lifecycle and implementation blockers are closed locally:
  session
  transport identity and management-loop-only access, callback/timer execution
  admission, PacketFramer budgets and a real fuzz target, LogicLoop
  concurrency/lifecycle, and Pipeline authentication with physical
  IO/management/logic loop handoffs. Pipeline revocation now wins even when a
  callback retains state, synchronous Logic-stage stop cannot destroy an active
  handler, and deterministic tests cover one exact AUTH-plus-command batch and
  a SessionManager submit/drain overlap with two live producers.
- [x] At the frozen Phase 4 candidate checkpoint, intent semantics resolved all
  25 active targets and 74 explicit verification paths while preserving 16
  documented frozen Core metadata
  exemptions. Configured-CMake dependency analysis proves the one-way
  production graph and rejects direct and transitive reverse edges.
- [x] Broadcast routing now enforces Router-only immutable plans, owner
  availability, exact rejection metrics/order, bounded fanout, and real
  multi-loop TCP integration contracts across eight reconnect cycles.
- [x] The repository CI definition contains six producer jobs, including Linux
  sanitizer/fuzz/TSan gates and both Windows Debug IOCP and Windows Release
  package execution gates, plus one aggregation-only evidence-set gate. The
  long-soak workflow writes structured exact-repeat manifests and the Phase 4
  benchmark workflow requires a paired Linux/epoll plus Windows/IOCP manifest.
- [x] Pre-hardening PR #4 head `0d62054e148a1c95793799eb88856363ac6843d3`
  has historical five-job evidence in successful `ci` run `29147391402` (#32).
  It is not current-candidate evidence after the remediation changes.
- [x] Commit and push functional candidate
  `5ebad2c1a4a9487437340935e21f7468140c7e8d`; validate it through six main-CI
  producers plus `gamenet.ci_evidence_set.v1` in run `29160903594`, exact
  repeat-50 evidence in `29161167423`, and the paired Phase 4 benchmark gate in
  `29161168417`.
- [x] Move PR #4 to Ready and merge under explicit owner authorization without
  replacing the verified candidate; verify the merge tree, pass exact-commit
  main CI `29168786199`, and publish annotated tag `v0.2.0-phase4-preview`,
  checksums, and the formal GitHub Preview Release. Independent submitted review
  remained absent and is recorded as a process limitation.
- [ ] HTTP, RPC, UDP/KCP, TLS, coroutine, and a formal all-in-one pipeline library
  remain deferred until separately promoted.

Before promoting a later Preview or stable release, keep the Linux and Windows
CI gates green and record any missing soak, race-oriented, or platform-specific
verification as its own immutable validation step.
