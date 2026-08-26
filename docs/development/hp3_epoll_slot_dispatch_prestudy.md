# HP3 Epoll Slot Dispatch Prestudy

Date: 2026-08-25

Decision: `DEFER`

Evidence class: development-only portable decoder, non-promotion

## Scope And Lifecycle

The opt-in prototype packs slot index plus generation into a 64-bit token,
resolves a fixed slot directly, and merges repeat events through per-slot batch
epoch plus notice index. It owns fixed slot/free/notice storage and a control-
plane fd map, but no epoll fd, Channel, EventLoop, socket, callback or payload.

All operations are constructing-owner-only; the prototype exposes no cross-
thread wakeup. Decode invokes no callback. A later EventLoop-style dispatch
must call `isCurrent`, allowing callback re-entry to cancel/reuse another slot
without reaching an old or replacement target. Stop rejects registration and
requires exact cancellation plus an empty batch before shutdown.

## Correctness Evidence

The focused Windows MSVC Release contract passed 1/1, and its MSVC Debug
AddressSanitizer run passed 1/1. It covers capacity and malformed options,
packed identity, same-generation interest update, remove/reuse generation,
wakeup/malformed/stale/filter behavior, O(1) duplicate merge, finite batch
budget, epoch wrap, exact cancel, active-batch invalidation, foreign-owner
rejection, stop and zero residue.

## Development Measurement

Ten sequential Release samples used 1,024 active sources and 200 decode
iterations. Each sample ran a unique-event batch and a factor-four duplicate
batch after an unrecorded warmup.

| Scenario | Current model median | Slot candidate median | Median paired improvement |
| --- | ---: | ---: | ---: |
| 1,024 unique events | 22,514,850 ns | 717,900 ns | 96.80% |
| 4,096 events / 1,024 unique | 88,992,500 ns | 2,016,650 ns | 97.755% |

All samples matched delivered-notice counts and checksums and reached zero
residue. The current model recorded one fd-map lookup per native event and a
linear merge scan; the candidate recorded one slot probe per resolvable event,
zero wait hash lookups and zero merge probes.

## Missing Evidence And Decision

The measurement is intentionally an algorithm comparison, not a real backend
benchmark. This Windows host exercised no Linux epoll descriptor, registration,
wait, level-trigger drain-to-EAGAIN behavior, kernel stale-token delivery,
cache counters or end-to-end P99/P999 latency. It also lacks clean fixed-lab
pairing and confidence intervals.

Accordingly the result is `DEFER`, not `KEEP-EXPERIMENTAL` or `INTEGRATE`.
Production epoll remains unchanged, formal HP3 remains `planned`, and the one
prestudy slot is released. A future formal slice must run the real Linux path
and the existing readiness/EventLoop lifecycle contracts from scratch.
