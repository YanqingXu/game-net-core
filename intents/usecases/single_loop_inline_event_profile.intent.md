---
status: active
target: gamenet_single_loop_inline_echo
migration_source: native
promote_gate: none
artifact_kind: example
migration_mode: native
source_commit: none
source_paths: none
---

# Use Case Intent: Single-Loop Inline Event Profile

## 1. Intent

Profile A proves the TCP-only `SingleLoopInlineEvent` / Network-only Runtime
Model as a runnable composition of existing components. One EventLoop owns
accept, every connection, framing, the lightweight handler, output admission,
and shutdown. It adds no LogicLoop and performs no cross-domain handoff.

The first slice remains under `examples/runtime_profiles`. Its support target,
headers, and executable are not installed or exported. A reusable installed
Profile API is deferred until at least two RTM-R1 Profiles prove common needs.

## 2. Composition Contract

- transport is TCP through `TcpServer`, `TcpConnection`, and
  `TcpTransportEndpoint`;
- I/O topology is exactly one caller-owned EventLoop and `TcpServer` is
  configured with zero worker loops;
- each connection owns one `PacketFramer` through its owner-loop connection
  context;
- every handler invocation runs inline on that same owner EventLoop;
- the handler receives an immutable framed payload and returns an immediate
  reply/continue/close decision;
- no LogicLoop, global executor, work-stealing path, blocking wait, future, or
  recursive inline fallback is introduced.

## 3. Bounded Work and Non-Blocking Rule

`PacketFramerOptions::maxFramesPerPush` and `maxFrameBytesPerPush` bound handler
calls and framed bytes in one dispatch. Remaining complete frames are resumed
by one same-owner queued continuation. Continuation admission uses the bounded
EventLoop normal queue; QueueFull, Shutdown, or OwnerUnavailable closes the
connection and never falls back to recursive inline draining.

Handlers must not block. The configured positive handler wall-time budget is
an enforcement boundary,
not a scheduling quantum: a handler cannot be preempted, but an observed
overrun is counted and the connection is closed before another frame is
handled. Handlers must not sleep, wait on I/O/futures/locks, poll another
executor, or perform unbounded work. The wall-time check detects a violation;
it does not make blocking safe.

## 4. Backpressure and Memory

- input retention is bounded by the framer payload/buffer limits and the Core
  input-buffer limit;
- output is admitted through the existing per-connection TCP hysteresis and
  hard limit;
- an encoded response that exceeds protocol or output admission closes with a
  typed protocol/overload reason;
- per-connection Profile state contains one finite framer, one endpoint, and
  one queued-continuation bit; no Profile queue grows with message count;
- metrics expose handlers, maximum handlers in one dispatch, continuations,
  continuation rejection, handler overrun/exception, protocol failure, output
  overload, connections, and the invariant zero cross-domain handoffs.

## 5. Ownership, Re-entry, and Shutdown

The caller owns the EventLoop and Profile object and constructs, starts, stops,
and destroys the Profile on that owner thread. TcpConnection owns and releases
its per-connection Profile state on the same thread. The shared callback gate
outlives individual callbacks but never extends admission after revocation.

The handler may re-enter owner-only Profile stop/transport operations. No lock
is held around it. After it returns, admission is rechecked before reply or
continuation. `stopGracefully()` first revokes new handler admission, then
delegates bounded connection drain and worker-free shutdown to TcpServer. Work
already executing may return, but no later frame begins. The completion future
must not be waited on from the owner loop.

Profile methods do not add a cross-thread facade. External coordination uses
the existing EventLoop executor/control APIs and then calls the Profile on its
owner.

## 6. Verification

- `tests/contract/runtime_model/test_network_only_profile.cpp` proves explicit
  zero-worker topology, callback owner identity, a 2/2/1 bounded five-frame
  continuation, ordered echo, zero cross-domain handoff, continuation
  saturation close, blocking-budget violation close, output-overload close,
  callback re-entry, and bounded shutdown completion.
- `tests/cmake/test_runtime_profile_contract.py` rejects installation/export,
  worker-loop drift, unbounded/recursive continuation, absent overload handling,
  or missing Profile A contract registration.
- `examples/runtime_profiles/single_loop_inline_echo.cpp` is the runnable
  lightweight echo composition used for the zero-handoff baseline.

## 7. Non-Goals

- no installed Runtime Profile type in this slice;
- no connection migration, logic shard, Tick scheduler, or business state;
- no claim that wall-time observation can preempt a blocking handler;
- no UDP, KCP, TLS, HTTP, RPC, coroutine, or experimental transport work.
