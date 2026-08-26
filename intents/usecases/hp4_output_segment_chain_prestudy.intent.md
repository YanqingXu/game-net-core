---
status: active
target: gamenet_hp4_output_segment_chain_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: native
source_commit: none
source_paths: none
---

# Use-Case Intent: HP4 Output Segment Chain Prestudy

Prestudy state: `DEFER`.

## Intent

This concluded `EXPERIMENTAL-PRESTUDY` slice evaluates a fixed-capacity
owner-local `OutputSegmentChain` whose
segments either own a moved string or share immutable storage, retain an exact
offset, and expose at most 16 views / 64 KiB for one vectored write.

The production Linux Buffer write path, Windows IOCP single-WSABUF segment
path, `TcpConnection::trySend`, hierarchical output budgets, TransportEndpoint,
BroadcastDispatcher, installed API and defaults remain unchanged baselines.

## Ownership, Capacity, And Admission

- one connection/EventLoop owner constructs, enqueues, prepares/completes a
  batch, stops, cancels and destroys one chain. Foreign-thread mutation is
  rejected; cross-thread callers would first own a value and marshal it through
  the existing bounded owner admission outside this prototype;
- construction allocates a fixed segment ring. Accepted enqueue returns
  `Accepted`; segment/byte capacity, stopped state, invalid shared slices and
  owner loss remain explicit. Rejection occurs before an owned rvalue is moved;
- an owned segment exclusively owns one moved string allocation. A shared
  segment retains `shared_ptr<const string>` plus offset/length. Neither form
  concatenates header and payload or copies an uncompleted suffix;
- accepted pending bytes never exceed the configured hard limit. Every byte
  is terminally completed or explicitly discarded exactly once;
- one immutable payload may be referenced by many owner-local chains. A
  broadcast owner batch validates route generation immediately before enqueue,
  publishes at most one batch observation and reports accepted/stale/rejected
  endpoints separately.

## Write, Partial Completion, And Re-entry

- when the chain was empty, the owner may prepare immediately after enqueue;
  the prototype never delays for a later turn merely to grow a batch;
- one prepared batch borrows stable segment suffixes until `completeBatch` or
  `failBatch`. It contains at most 16 views and at most 64 KiB total;
- only one batch may be in flight. Recursive/repeated prepare, cancellation
  during an in-flight batch, zero completion and completion beyond submitted
  bytes fail explicitly without corrupting offsets;
- partial completion advances offsets across one or more segments, removes
  only fully completed segments, and reports exact released bytes. A later
  batch starts at the remaining suffix address without copying it;
- prepare/complete invoke no user callback. A later high-water/write-complete
  callback may re-enter higher owner lifecycle only after mandatory byte and
  write-interest state has settled in a formal integration;
- begin-stop seals enqueue. In-flight work must first complete/fail; owner-only
  cancellation then destroys every remaining segment and records discarded
  bytes. Final settlement requires zero segments/bytes/in-flight views and
  `acceptedBytes == completedBytes + discardedBytes`.

## Platform And Evidence Boundary

The portable prototype produces view arrays shaped for `writev/sendmsg` or
multi-`WSABUF`, but invokes neither syscall. Only native Linux/Windows contracts
can validate syscall limits, completion storage, socket errors, write-interest,
backpressure, cancellation and callback ordering. Development timing compares
per-endpoint concatenation/copy with shared two-segment owner batches and cannot
close HP4 or add owned/shared public send overloads.

## Verification

- `tests/contract/tcp_connection/test_output_segment_chain.cpp` covers fixed
  capacity, owned/shared lifetime, no concatenation, 16-view/64-KiB limits,
  immediate prepare, FIFO, partial cross-segment completion, suffix address,
  one-in-flight/re-entry rejection, invalid completion, stop/cancel accounting,
  foreign owner, shared broadcast generation and zero residue.
- Existing IOCP segmented/partial-write, output-memory and broadcast integration
  contracts remain unchanged production regression requirements.
- `tests/cmake/test_hot_path_benchmark_contract.py` enforces isolated build,
  one active prestudy, non-installation and public/default absence.

## Non-Goals

- no real writev/WSASend integration, TcpConnection/TransportEndpoint/Broadcast
  edit, installed owned/shared API, budget redesign, public selector, HP4
  integration, version or release;
- no HTTP/TLS/UDP/KCP/RPC/coroutine scope;
- no promotion conclusion before HP0 and native cross-platform fixed-lab gates.

## Prestudy Decision

Windows MSVC Release passed the focused contract and five unchanged production
regressions; the focused Debug AddressSanitizer contract also passed. Ten local
samples at 256 endpoints x 100 iterations preserved equal checksums and zero
residue. For a 1,024-byte payload the segment candidate regressed elapsed time
by a median 65.10%; for 16,384 bytes it improved it by 94.07%. In both cases it
removed the modeled 25,600 allocations and all payload concatenation copies.

This is mixed, development-only evidence. The candidate does not issue a real
`writev` or `WSASend`, measure syscall count, exercise native completion
lifetime, or carry fixed-lab HP0 identity. The decision is therefore `DEFER`.
Production send/broadcast paths remain unchanged, formal HP4 remains planned,
and the one runtime prestudy slot is released.
