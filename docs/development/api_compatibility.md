# API Compatibility Policy

`api/public_api_manifest.json` is the v2 machine-readable compatibility
boundary for the 0.3 production-candidate line.
The frozen line installs as package version `0.3.0`; its CMake project version,
manifest package version, explicit `compatibility_line`, exact-version
consumer, and release label must agree.

The manifest classifies both exported CMake targets and installed public
headers. It also binds the candidate to the reconstructed
`v0.2.0-phase4-preview` snapshot by tag, full commit, repository-relative
snapshot path, and snapshot SHA-256. The hash uses UTF-8 content with normalized
LF line endings, so Windows and Linux checkouts verify the same snapshot
identity. All CI producers fetch full history: the guard also peels the tag,
enumerates its public headers/exported targets, verifies its package version,
and recomputes every frozen stable-header fingerprint from that Git object.
This prevents a change from replacing both the snapshot and its declared hash
with content that never existed at the release tag.

## Compatibility Classes

- `stable_core`: `GameNet::core` and its supported Core headers. Source
  compatibility is required within the 0.3 line. Any
  declaration-level drift must be reviewed and recorded by updating the
  manifest fingerprint. Removing or incompatibly changing a declaration also
  requires a migration decision in the release notes.
- `provisional`: the exported Phase 4 protocol, transport, session, logic, and
  broadcast targets and headers plus the initial metrics exporter/recorder
  headers remain available for evaluation but may change before 1.0. Metrics
  stay provisional until their allocation, contention, type, and
  enabled-overhead contracts are promoted.
- `platform_internal`: backend and operating-system integration headers are
  installed today for implementation reasons but are unsupported application
  interfaces. There is currently no separately exported internal target.

The project does not guarantee ABI compatibility before 1.0. Compiler, C++
standard-library, build-option, and runtime combinations are therefore not
interchangeable binary contracts. Consumers should rebuild against each 0.x
release.

## Change Procedure

Run the verifier and emit the deterministic historical diff:

```text
python tests/api/test_public_api_manifest.py
python tools/compare_public_api_manifest.py \
  --compatibility-baseline api/baselines/v0.3.0-api-r1-reviewed.json \
  --fail-on-compatibility-decision \
  --fail-on-stable-surface-review \
  --compatibility-output public-api-compatibility-diff.json \
  --output public-api-diff.json
```

The guard rejects missing, newly unclassified, or multiply classified public
headers or targets; package-version and compatibility-line drift; a stale or
tampered or tag-inconsistent historical snapshot; and any unrecorded stable
Core declaration change. Comments and whitespace are excluded from the stable
fingerprints.

The diff records target and header additions, removals, category moves, and
stable-header fingerprint changes in sorted JSON. Its summary separates two
questions:

- `stable_surface_review_required` is true for any change touching the stable
  surface, including an additive promotion.
- `compatibility_decision_required` is true when a removal, downgrade, or
  stable fingerprint change occurs inside the same compatibility line.

Cross-line changes such as the current 0.2 preview to 0.3 candidate comparison
still require review, but they do not claim to violate the 0.3-within-line
promise. The raw changes remain present in the evidence even when the decision
flag is false. The blocking gate separately compares the manifest with the
API-R1 reviewed 0.3 snapshot. That comparison must remain a zero diff; stable
header/target additions, removals, moves, and fingerprint changes all fail the
stable-surface gate. The separate compatibility output archives this same-line
result instead of conflating it with the historical 0.2-to-0.3 diff.

The installed `GameNetCoreConfigVersion.cmake` uses CMake
`SameMinorVersion`: 0.3 consumers cannot silently resolve 0.2 or 0.4 packages.
The install fixture compiles one consumer against all stable Core headers and
`GameNet::core` only, plus a separate provisional consumer. It also executes
positive 0.3 and negative 0.2/0.4 version probes.

A deliberate stable API change must update the public contract, direct tests,
manifest fingerprint, and release notes in one reviewed change. Updating a
fingerprint alone is not evidence that the change is compatible.

## Current M1 Additive Review

The 2026-07-27 M1 lifecycle closure adds `TcpClientControl.h` and
`TcpConnectionClose.h` to `stable_core`. It also updates the recorded
declaration fingerprints for EventLoop/Poller/Socket, TCP callbacks,
TcpClient/TcpConnection/TcpServer, and callback-exception source reporting.
Direct lifecycle, saturation, completion-drain, close-reason, detached-handle,
and install-consumer contracts cover the new surface.

The deterministic comparison against the 0.2 Preview reports a stable-surface
review requirement and no within-line compatibility-decision requirement.
That flag is a compatibility-line classification, not a claim that every
0.2-to-0.3 declaration change is additive. The cross-line decisions are
recorded by API-R1 below.

The primary Linux CI producer writes
`ci-evidence/public-api-diff.json` before its evidence manifest is built. The
existing per-job evidence artifact therefore retains the diff for 90 days and
the aggregate evidence gate covers the producer artifact.

## Current M3-G5 Additive Review

M3-G5 adds `Acceptor::setIocpAcceptDepth()` /
`Acceptor::iocpAcceptDepth()` and `TcpServer::setIocpAcceptDepth()` to the
stable Core source surface. Existing constructors and defaults remain source
compatible; applications that do not configure the option receive the bounded
Windows default depth of four, while non-Windows readiness behavior is
unchanged. Reconfiguration is rejected after listen/start, and `[1, 64]`
validation prevents an unbounded pool.

The `Channel.h` fingerprint also changes because its private Windows backend
state now carries the exact IOCP operation identity published for the current
active entry plus an allocation-free head/tail pair for bounded Accept
completion coalescing. This is not an application scheduling API and adds no
public method. No binary ABI promise exists before 1.0, so consumers rebuild
as already required.

Direct contracts are
`tests/contract/acceptor/test_acceptor_contract.cpp`,
`tests/contract/acceptor/test_acceptor_iocp_pool.cpp`,
`tests/contract/tcp_server/test_tcp_server_contract.cpp`, and
`tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`.
They cover the public option, finite default/configured depth, exact completion
identity, burst replenishment, Retry/Stop cancellation, synchronous failure,
callback re-entry, and final-drain convergence.

## Current M3-H1-A Additive Review

M3-H1-A appends three startup-validated count budgets to `EventLoopOptions`:
`maxActiveChannelsPerIteration`, `maxTimersPerIteration`, and
`maxControlCallbacksPerIteration`. M3-H1-B appends
`maxIocpCompletionsPerPoll`, validated in `[1, 64]`; non-Windows backends
retain but do not use it. Existing aggregate initialization remains
source compatible because the fields are appended after the earlier options
and retain conservative defaults. `EventLoopMetricEvent` and
`EventLoopMetricSample` likewise append active/timer/lifecycle phase events
and generic drained/remaining/oldest-ready/exhausted observations; existing
enumerator values and positional field meanings are unchanged.

The `TimerQueue.h` stable fingerprint changes only in its EventLoop-private
budgeted drain shape. Direct contracts are
`tests/contract/event_loop/test_event_loop_fair_budget.cpp`,
`tests/contract/event_loop/test_event_loop_control_saturation.cpp`,
`tests/contract/event_loop/test_event_loop_lifecycle_hub.cpp`, and
`tests/contract/timer_queue/test_timer_queue.cpp`. H1-B adds
`tests/contract/poller/test_poller_contract.cpp` and
`tests/contract/acceptor/test_acceptor_iocp_pool.cpp` for configured-width
packet, wakeup, Accept, read/write deferral, metric, and obligation evidence.

## Current M3-H2-A Additive Review

M3-H2-A adds `TimerOptions.h` to the stable Core surface and appends
`EventLoop::runEvery(Duration, Functor, RepeatingTimerOptions)`. The existing
two-argument overload is unchanged and remains fixed-delay. Fixed-rate callers
must opt in and choose a finite maximum consecutive catch-up count; zero skips
all missed cadence points. Adding an overload and a new options type is source
compatible within the 0.3 line. Direct cadence, bounded catch-up, cancellation,
and invalid-option contracts live in
`tests/contract/timer_queue/test_timer_queue.cpp`.

M3-H2-B adds `EventLoopSelectionPolicy`,
`EventLoopThreadPool::setLoopSelectionPolicy()/selectLoop()`, and
`TcpServer::setLoopSelectionPolicy()`. Existing `getNextLoop()` remains and
delegates to the configured selector; the default remains round-robin.
Least-connections accounting stays source-private between TcpServer and the
pool, while queue-lag observes only the synchronized pending-functor queue and
consistent-hash requires an explicit key. These are additive source-compatible
changes within the 0.3 line. Direct contracts live in
`tests/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp`; the
existing TcpServer stop/release suite enforces zero residual assignment load
before pool shutdown.

M3-H2-C adds the stable Core `DeadlineQueue.h` surface and appends optional
deadline-resolution and per-advance-budget fields to
`TcpServerAdmissionOptions`; existing aggregate initialization remains valid
because the new fields are trailing and preserve defaults. `DeadlineQueue` is
callback-free and owner-loop-only: consumers retain target ownership and use
generation-tagged values for cancellation. SessionManager/PlayerSession remain
provisional while adopting the same substrate. Direct Core contracts live in
`tests/contract/timer_queue/test_deadline_queue.cpp`; TcpServer and
SessionManager contracts verify bounded backlog continuation and cleanup.

M3-Q1-A adds the stable Core `TcpOutputMemoryBudget.h` surface, appends
loop/server/global overload results to `TcpSendResult`, and adds setup-time
`TcpServerOutputMemoryOptions` plus low-frequency snapshots. Existing
connection-local `Overloaded` semantics and the default round-robin server
surface remain available; new TcpServer instances additionally receive finite
64 MiB per-loop and 256 MiB per-server output budgets, while a process/global
budget is opt-in and explicitly shared by the caller. Every shared scope uses
atomic reservation rather than a per-send mutex, and later-scope rejection
rolls earlier scopes back before returning. This is an additive source change
within the 0.3 line, but the enum/header fingerprints remain part of the
required independent stable-surface review. Direct contracts live in
`tests/contract/tcp_connection/test_tcp_output_memory_budget.cpp`,
`tests/contract/tcp_connection/test_tcp_connection_cross_thread_send.cpp`,
and `tests/contract/tcp_server/test_tcp_server_output_memory.cpp`.

M3-Q1-B extends the provisional `BroadcastDispatcher` diagnostics with global
current/peak/rejection observations and a low-frequency per-owner snapshot.
The existing `outstanding().tasks` / `.bytes`, dispatch limits, rejection
reasons, and shutdown entry points remain source compatible; `tasks == 0`
continues to be the exact convergence marker. Owner registration is published
once, while repeated owner-task -> owner-byte -> global-byte reservation,
rollback, completion release, shutdown, and snapshots use atomic state without
the former per-task dispatcher mutex. This remains a provisional-surface
change and therefore does not alter a stable-header fingerprint. The direct
contract is
`tests/contract/broadcast/test_broadcast_outstanding_budget.cpp`.

M3-Q1-C adds stable Core `BufferRetentionOptions` /
`BufferRetentionSnapshot` and replaces the zero-argument Buffer constructor
with an explicit options constructor that retains a default argument, so
existing `Buffer buffer;` source remains valid. The default target is 64 KiB
plus the cheap-prepend region and the readable recovery threshold is 16 KiB.
Active readable bytes may still grow beyond that target; only a later
low-water observation performs an opportunistic, data-preserving trim. This is
an additive source change within the 0.3 line, but `Buffer.h` changes its stable
fingerprint and consumers must rebuild under the existing pre-1.0 policy.

The provisional PacketFramer options append nullable retention target and trim
threshold fields. Omitting them selects one maximum-size frame for both values,
preserving existing aggregate initializers and avoiding a reallocation for
ordinary maximum-size frames. Both components expose owner-thread-only
current/peak/trim snapshots; no cross-thread guarantee is added. Direct
contracts live in `tests/contract/buffer/test_buffer_contract.cpp` and
`tests/contract/protocol/test_packet_framer_budget.cpp`.

## Current M3-Q1-D Additive Review

M3-Q1-D adds the stable Core `NetworkMemoryRetention.h` surface. Its
cross-thread-safe process snapshot accounts only transport-internal pool, slab,
and fixed working storage that is not already represented by logical
pending-byte budgets: AcceptEx slot vectors, IOCP Poller completion workspaces,
and connection-local IOCP read chunks. Shared read-pool and shared-slab fields
are explicitly zero in the active design. AcceptEx bytes follow the final
shared pool-state lifetime, so an Acceptor stop cannot hide storage still
retained by completion leases.

The new header is additive within the 0.3 line and does not itself change an
existing stable declaration. IOCP platform-private storage moves two per-poll
arrays into the existing Poller-lifetime fixed workspace. API-R1 later removes
backend-only hooks from Poller's public section, so the final 0.3 Poller
fingerprint does change. Direct contracts live in
`tests/contract/acceptor/test_acceptor_iocp_pool.cpp`,
`tests/contract/poller/test_poller_contract.cpp`, and
`tests/contract/tcp_connection/test_tcp_connection_iocp_read_storage.cpp`.

## API-R1 Stable Core Review

API-R1 independently reviewed the complete proposed 0.3 stable Core surface on
2026-08-05. The historical diff against `v0.2.0-phase4-preview` contains 10
stable-header additions, four provisional-header additions, 19 stable-header
fingerprint changes, no removed headers, no category moves, and no target
changes. The exact decision record is
[`api-r1-stable-core-review.md`](../reviews/api-r1-stable-core-review.md), and
the historical diff is archived beside it.

The review initially rejected the surface. Remediation made backend socket
types a real stable wrapper, moved Poller/EventLoop IOCP hooks out of the public
contract, made queue/shutdown failure outcomes observable, froze owner-only
configuration at defined lifecycle boundaries, documented callback, ownership,
destruction, and raw-memory contracts, and added negative tests. The stable
install consumer now includes every manifest-classified stable header without
linking provisional targets.

The following 0.2-preview to 0.3 source breaks are deliberately accepted. They
do not weaken the within-0.3 promise:

- `Connector::start()` changes from implicit cross-thread marshaling to an
  owner-loop-only operation, and `setRetryEnabled()` is no longer `noexcept`;
- explicit options constructors can reject copy-list forms such as
  `T value = {}` even where direct/default construction remains supported;
- `Buffer::readFd` has changed exact member-function type, so code storing that
  member pointer must migrate;
- direct application construction/add/cancel of `TimerQueue` is withdrawn.
  Consumers migrate to EventLoop's timer facade, preventing a standalone queue
  from returning `Accepted` even though that loop never polls it.

The reviewed surface is frozen in
`api/baselines/v0.3.0-api-r1-reviewed.json`. REL-C1 binds that snapshot to
annotated tag `api-r1-approved-surface`, peeled commit
`9d2a5be0eb5439399f27c2f53ec1bf985c7de1d0`, and proves that the surface is
still a zero diff. The final candidate is a separate annotated tag,
`v0.3.0-rel-c1-refreeze-1`; its peeled commit is the authoritative
`CANDIDATE_SHA`. It supersedes
`v0.3.0-rel-c1-freeze@d3137f9298b47474ea96dc694d44c5c026710039`
after that candidate's aggregate verifier exposed a stale install-consumer
count; the reviewed surface itself did not change. This non-self-referential
binding does not create an ABI guarantee or a REL-D1 release decision.

## Current M3-Q1-E Additive Review

M3-Q1-E adds the owner-loop-only
`TcpConnection::memoryRetentionSnapshot()` diagnostic. The returned
`TcpConnectionMemoryRetentionSnapshot` reports the input/output Buffer
retained-capacity snapshots, connection-local transport read storage, its
lifetime peak, and the exact current total. It does not weaken thread affinity:
cross-thread diagnostics must post through the connection owner executor.
The direct compatibility and lifecycle evidence is
`tests/contract/tcp_connection/test_tcp_connection_lifecycle.cpp` and
`tests/contract/tcp_connection/test_tcp_connection_iocp_read_storage.cpp`.
