---
status: active
target: gamenet_hp3_epoll_slot_dispatch_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: native
source_commit: none
source_paths: none
---

# Use-Case Intent: HP3 Epoll Slot Dispatch Prestudy

Prestudy state: `DEFER`.

## Intent

This is the one active `EXPERIMENTAL-PRESTUDY` implementation slice after HP2
concluded `KEEP-EXPERIMENTAL`. It isolates the Linux epoll wait-side lookup and
same-batch merge algorithm: `epoll_event.data.u64` carries a slot-index plus
generation token, a fixed slot arena resolves it in O(1), and one per-slot
batch epoch merges duplicate masks without a linear notice scan.

The production `EpollReadinessPort`, `EPollPoller`, `Poller`, EventLoop active
batch, Channel layout, Linux default, Windows IOCP path and public API remain
unchanged baselines. The portable decoder prototype is not evidence that a
real Linux epoll syscall path has integrated or improved.

## Topology, Identity, And Capacity Contract

- one EventLoop owner constructs, registers, updates, cancels, decodes,
  observes, stops and destroys one arena; foreign-thread mutation is rejected;
- capacity and maximum notices per batch are finite, nonzero construction
  options; storage and the free-index stack allocate once at construction;
- fd→slot lookup is used only for register/update/cancel/diagnostics. Native
  decode extracts `slotIndex + generation` directly and performs no hash lookup;
- token zero is reserved for wakeup. The low 32 bits encode `slotIndex + 1` and
  the high 32 bits encode a nonzero generation;
- slot reuse advances generation before the replacement becomes current.
  Generation exhaustion fails closed; an old token cannot address the
  replacement target;
- register reports `Accepted`, `Capacity`, `Conflict`, `Invalid`, or `Stopped`.
  Cancel validates exact fd, target and identity before releasing the slot;
- interests may be disabled/re-enabled without changing generation. Error and
  close masks remain deliverable; removed read/write interests are filtered.

## Wait Decode, Merge, Re-entry, And Lifetime Contract

- `beginBatch` advances a nonzero batch epoch and clears only the notice view,
  not every slot. Epoch wrap performs an explicit bounded reset outside normal
  decode before reusing epoch one;
- one current native token resolves its slot by index, checks generation and
  active state, filters its mask, and appends at most one notice. Later events
  for that slot use `lastBatchEpoch + noticeIndex` to merge in O(1);
- zero/wakeup, malformed, inactive, replaced and interest-filtered tokens
  create no target notice and increment exact typed counters where applicable;
- decode invokes no callback, consumes no fd payload and owns no Channel. A
  notice is a borrowed target plus identity and mask valid only while
  `isCurrent` succeeds;
- an EventLoop callback may cancel/re-register a later target. Dispatch must
  revalidate the notice; the stale snapshot cannot reach the replacement;
- no operation is cross-thread. EventLoop wakeup remains the only production
  cross-thread backend operation and is not reimplemented by this prototype;
- stop rejects new register/update, preserves exact cancellation obligations,
  and reaches Shutdown only with zero registrations/notices.

## Performance Hypothesis And Evidence Boundary

Compared with the current fd-decoding + `unordered_map` lookup + linear notice
merge model, the arena should perform zero wait-side hash lookups, a bounded
number of slot probes equal to native events, and zero duplicate-merge scans.
The opt-in benchmark records lookup/merge work and elapsed time for current and
candidate decoders at duplicate and unique active populations.

Only native Linux can prove epoll registration/wait behavior, cache effects,
syscalls and end-to-end tail latency. Windows portable decoder results are
development-only and cannot close HP3, change the Linux default or satisfy the
HP0 5%/3% gate.

The portable Release/AddressSanitizer contract and local algorithm comparison
passed, but no native Linux epoll path ran in this prestudy. The conclusion is
therefore `DEFER`; the isolated source is retained as design evidence and the
single prestudy implementation slot is released.

## Verification

- `tests/contract/io_engine/test_epoll_slot_dispatch.cpp` covers finite
  capacity, identity packing, same-generation update, remove/reuse generation,
  stale/malformed/wakeup tokens, O(1) duplicate merge, interest filtering,
  batch budget, epoch wrap, exact cancel, active-batch invalidation, owner
  rejection, stop and zero residue.
- `tests/cmake/test_hot_path_benchmark_contract.py` enforces the one active
  prestudy, benchmark-only build graph, non-installation and public/default
  isolation.

## Non-Goals

- no production `EpollReadinessPort` edit, real epoll wrapper, public slot or
  backend selector, edge triggering, Channel layout change, Windows semantic
  equivalence claim, HP3 integration, version or release;
- no HTTP/TLS/UDP/KCP/RPC/coroutine scope;
- no promotion conclusion before clean native Linux fixed-lab evidence.
