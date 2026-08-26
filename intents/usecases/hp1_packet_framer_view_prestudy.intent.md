---
status: active
target: gamenet_hp1_packet_framer_view_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: native
source_commit: none
source_paths: none
---

# Use-Case Intent: HP1 PacketFramer View Prestudy

Prestudy state: `KEEP-EXPERIMENTAL` (2026-08-25). The implementation slot is
released; the target remains opt-in for later fixed-baseline replay.

## Intent

This is the one authorized `EXPERIMENTAL-PRESTUDY` runtime slice while HP0
fixed-lab evidence remains `DEFER`. It evaluates owner-local length-delimited
framing over a caller-owned contiguous readable region without copying payloads
into per-frame strings. It changes no installed header, public manifest,
production Core target, default Profile A recipe, or HP milestone status.

The prototype names are isolated in `gamenet::experimental::hp1`:

- `PacketView` synchronously borrows one payload subspan;
- `OwnedPacket` is the move-only cross-domain carrier produced by
  `PacketView::retain()`;
- `FrameVisitResult` reports status, consumed bytes, visited frame count,
  processed frame bytes, and continuation;
- `PacketFramerViewPrototype::visitFrames(span, visitor)` applies the existing
  frame-count and frame-byte budgets directly to the supplied readable span.

## Input And Differential Contract

- The input is one contiguous snapshot of a caller-owned Buffer readable
  region. The prototype never appends to, retrieves from, grows, or retains
  that Buffer.
- A no-callback preflight validates the same prefix, maximum payload, complete
  frame, frame-count budget, and frame-byte budget order as legacy `push()`.
  Therefore a valid frame followed by an oversized prefix fails before any
  visitor side effect, matching the legacy fail-closed batch result.
- Only bytes belonging to frames whose visitors returned normally contribute
  to `consumedBytes`. A partial prefix or payload remains unconsumed in the
  caller Buffer.
- `needsContinuation` is true only when a complete valid frame remains because
  a configured budget stopped the visit. A partial next frame does not request
  continuation.
- Empty payloads are valid borrowed views. Sticky frames preserve wire order.
- A readable region above `maxBufferedBytes` reports `BufferLimitExceeded`,
  faults the prototype, invokes no visitor, and consumes zero bytes.
- An oversized declared payload reports `FrameTooLarge`, faults the prototype,
  invokes no visitor for that batch, and consumes zero bytes.
- A visitor exception is contained as `VisitorException`; prior normally
  returned frames are reported consumed, the throwing frame is not. The owner
  integration must treat that status as terminal rather than retrying an
  arbitrary user side effect.
- Legacy `push()` and candidate `visitFrames()` are compared over partial,
  sticky, empty, oversized, budget, fault, reset, and randomized valid streams.

## Ownership, Threading, Re-entry, And Cross-Thread Rules

- One connection EventLoop owns the prototype, source Buffer mutation, visitor
  invocation, consumption decision, reset, and destruction. The prototype is
  unsynchronized and has no cross-thread method.
- The source Buffer exclusively owns its storage. `PacketView` owns nothing and
  is valid only until its visitor returns; it may not be saved, sent to another
  thread, or held across `co_await`.
- `PacketView::retain()` copies payload bytes exactly once into `OwnedPacket`.
  Rejected or unused views allocate and copy nothing.
- `OwnedPacket` owns its string bytes, is move-only, and may cross threads only
  through a later typed bounded admission mechanism. HP1 creates no mailbox.
- The visitor may re-enter higher-level owner-only stop/close operations, but
  may not mutate the source Buffer or the same framer. Nested visit and reset
  are explicitly rejected while a visit is active.
- No lock is held and no EventLoop task is posted by the prototype. Owner-local
  execution therefore adds zero generic post, wakeup, or handoff.

## Lifecycle And Failure Settlement

- Construction validates the same maximum payload, buffer, frame-count,
  frame-byte, and retention-option coherence accepted by current
  `PacketFramer` even though the view prototype owns no retained byte storage.
- Fault is sticky until an owner-local reset succeeds. Reset during a visitor
  is rejected and changes no state.
- The caller applies `Buffer::retrieve(consumedBytes)` only after visit returns
  and before any operation that may grow or trim the Buffer. On protocol fault
  or visitor exception, the experimental integration closes or explicitly
  discards the unread input; it does not silently loop.
- The prototype owns no queued work, retained span, callback, EventLoop,
  Buffer, connection, or shutdown obligation. Destruction therefore leaves
  zero retained packet/view state.

## Performance Hypothesis And Evidence Boundary

- Owner-local `allocations/frame` is zero when the visitor does not retain.
- Additional payload copy is zero; `retain()` performs at most one explicit
  payload copy.
- Owner-local generic posts, wakeups, and cross-domain handoffs are zero.
- The opt-in benchmark compares the unchanged legacy callback-copy path with
  the candidate over the same generated wire bytes and reports elapsed time,
  payload bytes, legacy copied bytes, candidate retained bytes, and checksums.
- All output is development evidence. Before HP0 closes, this slice can decide
  only `KEEP-EXPERIMENTAL`, `REJECT`, or `DEFER`; it cannot switch Profile A,
  add the planned installed API, satisfy 5%/3%, or output `INTEGRATE`.

## Verification

- `tests/contract/protocol/test_packet_framer_view.cpp` verifies borrowed and
  retained ownership, zero-allocation owner-local visits, partial/sticky/empty/
  oversized/budget/fault/reset behavior, preflight-before-callback failure,
  visitor exception settlement, nested visit/reset rejection, Buffer
  consumption, and differential legacy decoding.
- `tests/cmake/test_hot_path_benchmark_contract.py` verifies that this is the
  only active `EXPERIMENTAL-PRESTUDY`, that its target is benchmark-gated,
  non-installed, and registered with its exact contract and benchmark.
- Existing PacketFramer contracts and fuzz remain the legacy semantic baseline
  and must stay green; this prototype does not weaken them.

## Non-Goals

- no installed `PacketView`, `OwnedPacket`, `FrameVisitResult`, or
  `PacketFramer::visitFrames` API;
- no default Profile A change, TcpConnection change, mailbox, outbox, coroutine,
  segmented send, epoll slot arena, or backend selector;
- no claim of promotion, integration, fixed-lab evidence, or milestone closure.

## Prestudy Decision

Windows MSVC Release differential contracts and the MSVC AddressSanitizer
contract pass. Four development-only payload cases (32, 256, 4096, and 16384
bytes) each ran ten sequential samples with equal legacy/candidate checksums,
zero candidate allocations, and zero candidate payload-copy bytes. Observed
median elapsed improvement ranged from 31.99% to 74.97%.

Decision: `KEEP-EXPERIMENTAL`. Linux, libFuzzer integration, real Profile A
TcpConnection input-Buffer execution, fixed-lab observer counters, and the HP0
5%/3% gate are not present, so the HP1 milestone remains planned and no default
or installed surface changes.
