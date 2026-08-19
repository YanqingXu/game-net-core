---
status: active
target: gamenet_multi_io_queued_echo
migration_source: native
promote_gate: none
artifact_kind: example
migration_mode: native
source_commit: none
source_paths: none
---

# Use Case Intent: Multi-I/O Queued Event Profile

## 1. Intent

Profile B proves the TCP-only `MultiIoQueuedEvent` Network/logic-split Runtime
Model as a runnable composition of existing components. A base EventLoop accepts,
at least two TcpServer worker EventLoops own connections, and one distinct
caller-owned EventLoop executes logic. Framed network input crosses into the
logic domain through GameCommandQueue; results return only through each
connection's owner executor.

The support target, header, and executable remain under
`examples/runtime_profiles` and are not installed/exported. Profile A and B may
inform a later common API review, but this slice does not publish one.

## 2. Placement and Handoff Contract

- TCP uses TcpServer, TcpConnection, TcpTransportEndpoint, and PacketFramer;
- `ioThreads >= 2`; TcpServer selects network owners, and established
  connections never migrate;
- the logic EventLoop is distinct from the base accept EventLoop and every
  network worker;
- each command copies immutable payload plus transport id and route generation
  into the existing bounded GameCommandQueue;
- an atomic scheduled-drain gate admits one logic executor post for the first accepted command
  while unscheduled; concurrent/later producers merge behind it;
- each logic callback drains at most `maxCommandsPerDrain`; remaining backlog
  uses one queued continuation and never recurses inline;
- handler results are encoded on the logic owner, then posted through the
  route's captured connection owner executor.

Business/data work does not use EventLoop control or lifecycle sources to
bypass normal queue admission.

## 3. Generation, Failure, and Backpressure

Each connection owns one route generation. Disconnect/close revokes it before
connection context release. Logic validates id/generation before and after the
handler; the connection-owner output callback validates again before send or
close. A stale result is dropped and cannot reach a later connection.

GameCommandQueue returns Accepted, QueueFull, PayloadTooLarge, Stopped, or
StaleBinding. Queue/payload rejection closes the affected route with an
observable overload/protocol metric. Logic wake or continuation rejection is
Profile-terminal: admission is revoked, the finite backlog is discarded and
counted, and all routes receive a close request. Owner-output post rejection or
endpoint overload closes only that route. No rejection falls back inline or to
an unbounded queue.

Input/framer limits, GameCommandQueue command/byte/payload limits, per-drain
command budget, TcpConnection output limits, and fixed latency histograms bound
memory/work. Logic handler wall-time is measured; exception or configured
overrun closes only the matching route.

## 4. Lifecycle and Re-entry

The caller constructs/starts/stops/destroys the Profile on the base owner and
keeps the separate logic EventLoop alive throughout. Network callbacks may
re-enter connection-owner APIs but cannot touch logic-owned state. The logic
handler receives no endpoint and cannot directly mutate transport.

`stopGracefully()` revokes Profile and route admission, closes/discards the
bounded queue, requests route close, and begins TcpServer drain. Its network
future covers Core connection/worker convergence. Its logic future stays
pending while a scheduled/in-flight drain may still call the handler and becomes
ready only after such callbacks return; handler output after revocation is
dropped. Callers wait on neither future from its owning EventLoop.

## 5. Metrics and Baseline

The synchronized snapshot exposes connection/network-owner counts, typed queue
rejections, producer wake posts/merges/rejections, drain callbacks and
continuations, handler failures, stale input/output drops, output posts/results,
queue snapshot/high-water, cross-domain handoffs, owner violations, and fixed-
histogram network-to-logic and logic-to-network P99/P999 plus maximum queue age.

The first benchmark is directional: it records throughput, both handoff tails,
queue age/depth, coalescing, memory and shutdown for the exact Profile B
composition. It is not promotion evidence.

## 6. Verification

- `tests/contract/runtime_model/test_network_logic_split_profile.cpp` proves
  multi-worker placement, one logic owner, deterministic merged wakeup,
  two-command drain continuation, ordered replies, typed saturation and
  recovery, generation-stale output rejection, and two-future shutdown.
- `tests/cmake/test_multi_io_queued_profile_contract.py` rejects installation,
  fewer than two workers, non-GameCommandQueue/unbounded handoff, recursive
  fallback, missing generation checks, or direct logic-thread transport calls.
- `examples/runtime_profiles/multi_io_queued_echo.cpp` is the runnable TCP echo
  composition used by the directional Profile B baseline.
- `benchmarks/runtime_profiles/multi_io_queued.cpp` records the opt-in directional
  throughput, handoff-tail, queue, coalescing, and shutdown baseline.

## 7. Non-Goals

- no installed Profile API, connection migration, logic sharding, or fixed Tick;
- no SessionManager/authentication/business state requirement in this minimal
  Profile contract;
- no control-source admission bypass, work stealing, global executor, or
  unbounded queue;
- no UDP, KCP, TLS, HTTP, RPC, coroutine, or experimental transport work.
