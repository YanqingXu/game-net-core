# Runtime Profile Common-Capability Review

Review date: 2026-08-20

Scope: the four non-installed TCP Runtime Profile vertical slices at Profile A
`adb8b483`, Profile B `633d613`, Profile C `da57edc`, and Profile D `b3b184b1`,
plus the combined real-TCP lifecycle contract added after those checkpoints.

## Decision

Do not promote an installed Runtime Profile interface now.

The four Profiles prove one semantic family: explicit owner placement, bounded
admission, generation-safe transport return, observable overload, and terminal
shutdown. They do not yet prove one source-level configuration, handler,
metrics, or stop-result interface. A least-common-denominator base class would
erase the cadence, sharding, cell-retirement, and zero-handoff differences that
the architecture requires the project to keep visible.

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

## Compatibility Decision

- Keep all four Profile classes, options, metrics, handlers, stop handles,
  examples, and benchmarks non-installed.
- Reuse the already installed lower-level primitives; add no new stable target,
  header, enum, factory, backend selector, or ABI surface.
- Keep `ConnectionPlacementPolicy` separate from `LogicShardPolicy`; do not
  infer logic placement from a network-worker index.
- Treat the normalized integration observation as a test vocabulary only. It
  is not a proposed runtime API and must not enter production headers.
- Reconsider an additive installed surface only after at least two independent
  non-example consumers need the same typed construction and stop protocol,
  and paired Linux/Windows evidence shows that the abstraction does not add a
  packet-path allocation, virtual dispatch, hidden wakeup, or lifecycle gap.

Disposition: `NO-PROMOTION`; continue implementation through lower-level
capabilities and explicit Profiles. This closes the current common-capability
review without freezing main or blocking the separate ARCH-G1 independent
review.
