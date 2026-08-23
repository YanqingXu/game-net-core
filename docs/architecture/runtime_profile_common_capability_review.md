# Runtime Profile Common-Capability Review

Review dates: 2026-08-20 (first review), 2026-08-23 (M5 re-review)

Scope: the four non-installed TCP Runtime Profile vertical slices at Profile A
`adb8b483`, Profile B `633d613`, Profile C `da57edc`, and Profile D `b3b184b1`;
the combined real-TCP lifecycle contract at `1c4c58f`; and the independent
private `gamenet-game-gateway` closure at `0a8fe1e` against installed Core
`736a090`.

## M5 Decision

Do not promote an installed Runtime Profile interface now.

Disposition: second `NO-PROMOTION`.

First-review Disposition: `NO-PROMOTION`; M5 independently repeats rather than
retroactively replacing that historical decision.

M3 supplied the missing real-consumer evidence: Queued Event and Sharded Hybrid
both ran as true-TCP gateway compositions on Linux/epoll and Windows/IOCP, and
the exact Sharded Hybrid gateway completed its one-hour replay/fault run. That
consumer used only installed GameNet 0.3.0 targets. Its feedback ledger found no
missing broadly reusable capability and requested no Core source-private helper
for either Runtime model.

The new evidence therefore strengthens the lower-level boundary instead of
authorizing a new one. `TransportEndpoint` is already installed and reused.
Admission, stop, shard, and cadence concepts still have different owner,
obligation, failure, and retirement semantics. M5 adds no target, header, enum,
factory, backend selector, or ABI surface, and no empty v0.4 release is created.

## M5 Candidate Audit

| Candidate | Evidence comparison | M5 disposition |
| --- | --- | --- |
| existing `TransportEndpoint` | Profiles A/B/C/D and the gateway use the installed endpoint plus captured `EventLoopExecutor`; the gateway adds its own binding-aware capability where business replacement policy needs another generation check | reuse as-is; no new Runtime alias or wrapper |
| typed bounded `LogicExecutor` admission | Profile B uses coalesced event drains, C admits without a per-command wake and drains only on fixed-rate ticks, D owns independent cell FIFOs with event/fixed no-overtake, while the gateway uses installed `LogicLoop` for one path and a gateway-owned cell executor for another | `NO-PROMOTION`; common names do not imply the same Accepted obligation or terminal failure scope |
| waitable monotonic `RuntimeStopFuture` | A has one network future; B has network plus logic-drain completion; C adds cadence-post publication and timer retirement; D aggregates all cell callbacks/timers; the gateway exposes synchronous aggregate stop around private owners and a Core network future internally | `NO-PROMOTION`; a common future would either erase obligations or become an untyped bag |
| shard types | Core D hashes key kind plus bytes; the gateway hashes its validated business key bytes and owns its routing grammar and duplicate/session policy | `NO-PROMOTION`; key vocabulary and invalid-key policy are consumer-specific |
| cadence types | C supports skip-missed or bounded catch-up; D and the gateway Hybrid use a fixed-rate timer within a cell-local no-overtake protocol; installed `LogicLoop` remains fixed-delay compatibility behavior | `NO-PROMOTION`; cadence cannot be separated from queue ordering, catch-up, and retirement semantics yet |

The prohibited catch-all shapes remain prohibited:
`UniversalGameServer`, `RuntimeProfileFactory`, `AnyTransportAnyLogicRuntime`,
and one Server class per strategy combination.

## Proven Common Capabilities

| Capability | A | B | C | D | Reusable installed primitive |
| --- | --- | --- | --- | --- | --- |
| TCP listen/start/stop | yes | yes | yes | yes | `TcpServer` |
| owner-loop connection state | one owner | selected network owner | selected network owner | selected network owner | `EventLoop`, `TransportEndpoint` |
| framed immutable input | inline | bounded queue | bounded tick queue | bounded cell FIFO | `PacketFramer` |
| generation-safe output return | same owner | logic to captured owner | tick owner to captured owner | cell owner to captured owner | `TransportEndpoint::ownerExecutor()` |
| finite admission and overload | dispatch/output | command/byte/drain | command/byte/tick | per-cell command/byte/lane | existing typed results and budgets |
| terminal graceful stop | network | network + logic | network + timer/logic | network + all cells | existing lifecycle primitives |

The combined
`tests/integration/runtime_model/test_tcp_runtime_profiles.cpp` contract drives
the same framed TCP echo, client close, admission seal, and graceful stop on the
configured epoll or IOCP backend. It compares only a narrow normalized outcome:
one echoed command, one retired connection, one handler/reply, drained network
and logic obligations, no owner/generation violation, and the expected handoff
model. It separately requires Profile A to remain zero-handoff, B/C/D to perform
the two explicit cross-domain transfers, C to retire its cadence, and D to
retire both logic cells without owner migration or order violation.

High-repeat execution exposed a Profile C shutdown race in which the logic-stop
future could capture the sentinel cadence-post result before the requesting
thread published the actual admission outcome. The Profile now makes result
publication a prerequisite of logic-stop completion; an owner Tick that retires
the timer first records the already-converged path as accepted. This reinforces
the review's requirement that stop semantics remain Profile-specific.

## Differences That Must Remain Explicit

| Dimension | A | B | C | D |
| --- | --- | --- | --- | --- |
| handler context | session/payload | session/payload | tick/session/payload | shard+tick/session/payload |
| dispatch trigger | inline + continuation | coalesced queue drain | fixed-rate tick | event drain + fixed-rate tick |
| placement input | one loop | I/O thread count + one logic loop | I/O thread count + one tick loop | connection policy + N logic cells + shard key |
| stop shape | network future | network + logic future | network + cadence/logic future | network + aggregate cell future |
| overload scope | connection | route or profile wake | route or timer/profile | route/cell or profile timer |
| metrics | dispatch | queue/handoff | cadence/jitter | per-cell/order/shard |

These are behavioral contracts, not cosmetic naming differences. Collapsing
them behind a single virtual `RuntimeProfile`, variant options object, generic
callback, or generic metrics bag would create invalid states and hide ownership
and terminal obligations.

## Ownership, Re-entry, and Cross-Thread Answers

- The base/accept EventLoop owns each Profile lifecycle call. TcpServer-selected
  network loops own connection, framing, endpoint mutation, and route
  revocation. Caller-owned logic loops outlive B/C/D stop and destruction.
- Connection context releases its finite framing/route state on the network
  owner. Profile shared state owns bounded queues, metrics, callback gates, and
  stop promises. C/D timer metadata retires on the corresponding logic owner.
- Router and handler callbacks may re-enter stop. No callback runs under a
  queue/route lock; admission and generation are revalidated after it returns.
- Every cross-thread operation uses an installed typed bounded queue or
  `EventLoopExecutor`; output returns through the captured endpoint owner and
  is generation-checked there. No inline or unbounded rejection fallback exists.
- The exact Core verification is
  `tests/integration/runtime_model/test_tcp_runtime_profiles.cpp`; the independent
  consumer verification is recorded by
  `docs/development/m3_gateway_closure_2026-08-23.md`. The M5 documentation/API
  boundary is guarded by `tests/cmake/test_migration_status_contract.py`.

## Compatibility Decision

- Keep all four Profile classes, options, metrics, handlers, stop handles,
  examples, and benchmarks non-installed.
- Reuse the already installed lower-level primitives; add no new stable target,
  header, enum, factory, backend selector, or ABI surface.
- Keep `ConnectionPlacementPolicy` separate from logic sharding; do not infer
  logic placement from a network-worker index.
- Treat the normalized integration observation as a test vocabulary only. It
  is not a proposed Runtime API and must not enter production headers.
- Publish the Profile load-selection guide as an official recipe guide. Profile
  D's real-sharding evidence gate is satisfied, but it remains provisional and
  non-installed under this decision.
- Reconsider an additive installed surface only after another independent
  non-example consumer needs the same typed construction or stop protocol and
  paired Linux/Windows evidence shows that it adds no packet-path allocation,
  virtual dispatch, hidden wakeup, or lifecycle gap.

This closes M5 without freezing main. Because no public capability was
promoted, v0.4.0 is not published; the next deliverable capability may take the
v0.4 version number.
