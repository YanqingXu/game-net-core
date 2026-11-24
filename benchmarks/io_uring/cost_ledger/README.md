# Saturated owner CPU cost ledger

This is a bounded diagnostic, not a 24h/72h promotion gate. Frozen runtime baseline:
`6004293e0492819e062326032a5391eda6e83229`. Diagnose before changing runtime;
compare applies the retained single-line `recv-poll-first.patch` ONLY to a
temporary local clone used to build the `after` package (Linux >=5.19 required).
Both committed runtime trees must equal the baseline. Archive and verify the
derived Engine source against that exact one-line transformation. This tests the
official recv poll-first hint without imposing a new kernel requirement on the
library or quietly shipping a benchmark hypothesis. Never merge main.

Invariants: owner affinity; one target terminal per accepted operation; cancellation
CQEs are bookkeeping; notice consumption precedes slot reuse; independently owned
admission payload; mailbox FIFO; physical stop; bounded bytes/operations/dispatch.
Pump stays at 16 notices, Core budgets and backpressure thresholds are unchanged.

Three separate observations:

* Scored installed Release: five interleaved fixed-work blocks (4096 rounds,
  64 peers, 4 KiB request/reply); block zero is warmup. Both epoll and io_uring
  use the same consumer, flags, affinity and socket policy. Fixed-time timer
  windows are 2 seconds, five blocks each of saturated and 1000-us paced traffic.
  Finish the outstanding round at the boundary; report actual duration/count.
  The 32768-round capacity is preallocated and exhaustion fails, never truncates.
* Unscored perf/strace: owner-thread, monotonic measurement window, 8192/512
  rounds. Kernel/user leaf domains and nearest-module stack contexts are disjoint;
  inclusive caller percentages must not be added. Unknown samples stay unknown.
  CPU sampling measures on-CPU execution, **not** runnable delay or blocking.
  Existing scheduler switch/wakeup diagnosis remains separate evidence.
* Unscored allocation/copy observer: same sources in an independent `-g`,
  no-builtin-memcpy/memmove/memset build, non-PIE diagnostic executable. Bounded
  owner-only TLS call-site table, enabled by owner handshakes after warmup and
  disabled after all echo replies. Overrides C++ allocation/deallocation and
  linker-wraps explicit memory calls; unchanged library semantics. Symbolize
  return PCs with retained executable + addr2line. Direct malloc, prebuilt DSO
  internals and compiler-generated/inlined copies are not counted. Counts are
  observed lower bounds, NOT heap residency or CPU-cost percentages. User/system
  CPU and context switches here describe this instrumented run, not scored data.

The diagnostic window includes its two control handshakes and possibly residual
warmup completions. Never assert exact two CQEs/SQEs per echo from these totals.
Full-lifetime native submissions/terminals and physical-zero ledgers still balance.

Archive all inputs, Git blobs, compiler caches, installed libraries, executables,
raw ledgers, profiler outputs, contract JUnit and hashes. Replay independently
with the Actions head SHA; cross-check source blob inventory against Git.
Negative trials remain evidence. No gain is not grounds for widening budgets,
dropping copies across ownership boundaries, or adding advanced io_uring modes.
