# HP2 SPSC Mailbox And Owner-Outbox Prestudy

Date: 2026-08-25

Decision: `KEEP-EXPERIMENTAL`

Evidence class: development-only, non-promotion

## Scope And Boundary

The prestudy implements a fixed-capacity `SpscMailbox<T>`, a generation-safe
coalesced `SpscMailboxSource<T>`, and move-only network→logic / logic→network
`DataPlaneCommand<OwnedPacket>` values only in the default-off
`GAMENET_BUILD_BENCHMARKS=ON` graph. It is not installed or exported and does
not register a production EventLoop lane or replace Profile B's
`GameCommandQueue` and owner-executor output posts.

## Lifecycle Answers

| Question | Prestudy answer |
| --- | --- |
| Owner thread | The consumer owner constructs, drains, validates routes, begins stop, cancels, detaches and destroys. Exactly one named producer uses each copied handle. |
| Ownership/release | Fixed slots own accepted move-only commands until owner processing or explicit cancellation. `OwnedPacket` owns payload bytes; no `PacketView` crosses the domain. |
| Callback re-entry | The visitor holds no mailbox lock and may perform higher-level owner behavior. Recursive drain of the same source is rejected. Visitor exception cancels the current item and seals admission. |
| Cross-thread operation | Producer release-publishes a constructed slot; consumer acquire-observes it. A generation and active-call gate linearize detach against stale handles. |
| Shutdown settlement | `beginStop` rejects admission. Owner drains or explicitly cancels every accepted slot; final settlement requires zero depth, clear notification state, no producer call and `accepted == processed + cancelled`. |

## Correctness Evidence

Windows MSVC Release passed both focused contracts and the existing Profile B
contract. The focused MSVC Debug AddressSanitizer build passed both HP2
contracts. Covered cases include:

- capacity, power-of-two and overflow-safe matrix-budget rejection;
- move-only FIFO/wraparound, batch drain, QueueFull/Stopped rejection before
  moving caller state, and a 100,000-item real producer/consumer ordering run;
- visitor exception containment and exact cancellation accounting;
- burst coalescing, bounded continuation, recursive drain rejection and both
  deterministic sides of the clear/recheck producer race;
- detach-generation invalidation, OwnerUnavailable, route rollover, owner
  outbox validation and zero terminal residue.

Intent metadata/semantics/consistency, hot-path governance, migration-status,
build-governance and public API manifest guards passed.

## Development Benchmark

Ten sequential local Release samples used 200,000 messages, 256-byte payload,
capacity 4,096 and drain batch 64. Every sample used one unrecorded warmup,
reported equal checksums and zero shutdown residue, and kept the SPSC candidate
at zero queue allocator calls, zero locks and zero generic posts.

| Metric | callback+mutex | callback+SPSC |
| --- | ---: | ---: |
| Median elapsed | 80,171,500 ns | 61,130,950 ns |
| Median paired elapsed improvement | — | 26.60% |
| Median queue allocator calls | 912 | 0 |
| Generic output posts/sample | 200,000 | 0 |
| Median mean queue age | 2,814.9 ns | 7,638.4 ns |

This burst benchmark has no rate controller or fixed latency SLO. Queue age
varied substantially and its median regressed, so the result fails the
guardrail needed for promotion even though throughput and structural costs are
promising. Raw samples, fixed observers, confidence intervals, Linux pairing
and a clean exact commit were not retained.

## Decision

The deterministic contracts and structural cost removal justify retaining the
isolated prototype as `KEEP-EXPERIMENTAL`. Formal HP2 remains `planned`, the
production/API graph is unchanged, and the single prestudy implementation slot
is released. After HP0 closes, any formal HP2 slice must replay this candidate
under rate-controlled fixed-lab loads and meet the full 5%/3% gate from scratch.
