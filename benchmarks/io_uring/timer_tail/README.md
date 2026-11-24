# Timer-tail attribution (development evidence only)

This experiment first reproduces deadline lateness on the unchanged runtime,
then compares a justified change against `66a106fe3c972d3642fcc15857b9daaab728e265`
on one native Ubuntu host. It is not a 24 h / 72 h promotion gate.

## Invariants and ownership

- The EventLoop owner runs all timer and message callbacks. The peer thread only
  uses sockets and the existing queued owner-control handshake.
- Server, connection, cancellation and physical-retirement logic is unchanged.
  The installed consumer still verifies bytes, sequence, EOF, graceful drain,
  native terminal accounting and zero residue.
- Core phase order and all budgets are unchanged (io_uring notice budget: 16).
- The diagnostic observer is consumer-local, owner-thread-only, bounded and
  preallocated. It retains numbers, never borrowed metric pointers. It is
  destroyed after the loop and its callbacks have stopped. A callback span
  permits nesting without changing re-entry behavior. No observer is installed
  in the uninstrumented executable.
- Tests: `test_timer_evidence.py`, the existing eleven io_uring contracts and,
  for a Core fix, timer/EventLoop/IoEngine contracts and the normal platform CI.

## Reproducers and measurements

1. Idle one-shot phase scan: 100/250/500/750/900 us relative deadlines, 400
   samples each. No network or foreign-thread wakeups. Also 3,000 absolute
   1 ms cadence ticks. Do not interpret these as real-time guarantees.
2. Installed epoll and io_uring: identical 64 connections, 4 KiB frames,
   warmup, pinned owner/peer CPUs, fixed work and timer cadence. Saturated and
   paced (1 ms between rounds) traffic are separate cases. Timer sample counts
   and exposure durations are reported; fixed work is not equal duration.
3. Uninstrumented trials are scored separately from traced trials. Link-time
   wrappers record poll entry/exit, requested timeout, result and owner CPU
   clock. Existing public EventLoop metrics mark phase ends; consumer callbacks
   have explicit spans. Wrappers do not change syscall arguments or errno.
4. Optional kernel scheduler events are attempted in a separate traced trial.
   Failure/unavailable events must remain visible. Wall-minus-thread-CPU time
   alone does not distinguish blocked wait, runnable delay or IRQ/steal time.

Attribution partitions the deadline-to-callback interval into poll, message
callback and remainder (in that priority, no overlapping sums). Phase markers
are supporting context, not fabricated phase-start timestamps. Report CPU and
wall time separately. A timer deadline spent in a rounded poll wait is distinct
from a busy I/O phase and from unexplained scheduling delay. No claim about
fixing every rare maximum follows from eliminating one reproducible cause.

Evidence binds the runtime and observer Git identities, installed archives,
compiler flags, native host/affinity, contracts, raw ledgers and exact inventory
hashes. A new schema prevents interpreting old timer ledgers as new evidence.
Do not change old verifier semantics or compare numbers from different hosts.

The initial v1 reproduction is Actions `45351361451` (observer `e58d765`,
unchanged runtime `66a106f`). V2 adds separately recorded kernel scheduler
traces for idle, saturated and paced cases on each package/backend. Its parser
requires matched switch/wakeup events and partitions lag into blocked,
runnable and scheduled intervals. Poll/message context overlaps that partition
and must not be added to it. Scheduled time can include IRQ/steal time; it is
not a substitute for thread CPU clocks. A captured trace with missing/lost
events is explicitly unavailable, not usable attribution.

The [completed native audit](../../../docs/development/io_uring_timer_tail_audit.md)
records the reproducer, exact before/after identities, same-host results,
scheduler decomposition, artifact hashes and replay commands. The verifier also
requires the complete five-case scheduler inventory for each package; missing
or duplicated cases cannot silently pass as a complete comparison.

Reference: Linux documents `epoll_wait`'s integer-millisecond timeout and
possible rounding/scheduler overruns, and `epoll_pwait2`'s timespec timeout:
<https://man7.org/linux/man-pages/man2/epoll_wait.2.html>.
