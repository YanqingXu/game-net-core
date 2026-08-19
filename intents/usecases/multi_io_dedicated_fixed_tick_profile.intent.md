---
status: active
target: gamenet_multi_io_fixed_tick_echo
migration_source: native
promote_gate: none
artifact_kind: example
migration_mode: native
source_commit: none
source_paths: none
---

# Use Case Intent: Multi-I/O Dedicated Fixed Tick Profile

## 1. Intent

Profile C proves the TCP-only `MultiIoDedicatedFixedTick` Runtime Model as a
runnable composition of existing components. A base EventLoop accepts, at least
two TcpServer worker EventLoops own connections, and one distinct caller-owned
logic EventLoop owns the authoritative fixed-rate tick. Network input enters the
existing bounded GameCommandQueue and is handled only by tick callbacks.

The support target, header, echo, and benchmark remain non-installed (not installed). This
slice does not modify or relabel the installed LogicLoop, whose compatibility
timer remains fixed-delay periodic drain rather than this Profile's cadence.

## 2. Cadence Contract

`FixedTickCadence` has two reviewed values:

- `FixedRateSkipMissed`: preserve the epoch but skip every cadence point missed
  by an overrun; `maxCatchUpTicks` must be zero.
- `FixedRateBoundedCatchUp`: preserve the epoch and replay at most the positive
  configured `maxCatchUpTicks` consecutively before skipping to a future point.

Both map to TimerQueue `RepeatingTimerMode::FixedRate`. No fixed-delay mode,
per-command wakeup, inline recursion, busy wait, or control/lifecycle-source
bypass is allowed. `tickInterval`, `maxCommandsPerTick`, handler/tick wall-time,
queue/input/output limits, and catch-up count are finite and validated.

The Profile records callback tick sequence separately from logical cadence
index. Every callback has one scheduled deadline, start jitter, catch-up flag,
duration, bounded command batch, and queue snapshot. Mirror accounting follows
the same fixed-rate skip/catch-up rule as TimerQueue and is contract-tested with
a deterministic overrun.

## 3. Placement and Handoff

- established TCP connections never migrate from the TcpServer-selected owner;
- each network producer copies immutable payload plus transport id and route
  generation into GameCommandQueue and never schedules an event-driven drain;
- one logic tick drains at most `maxCommandsPerTick` in queue order;
- the handler receives immutable tick context and no endpoint;
- encoded output posts only through the route's captured owner executor, with
  generation validation before the handler, after it, and on the network owner.

## 4. Failure, Backpressure, and Lifecycle

QueueFull or PayloadTooLarge closes only the affected route with typed metrics.
Timer setup failure is Profile-terminal. Handler exception or configured
handler overrun closes only its route; a tick wall-time overrun is recorded and
the cadence policy determines catch-up/skip without changing execution domain.
Owner-output rejection or endpoint overload closes only that route.

The caller starts/stops/destroys the Profile on the base owner and keeps the
logic EventLoop alive through Profile destruction. `stopGracefully()` revokes
admission and route generations, closes/discards and counts queued work, begins
TcpServer drain, and retires the repeating timer on the logic owner. The
separate logic future remains pending while cadence setup/retirement or a committed tick may
still invoke the handler. A callback already inside the handler may finish, but
its output is stale after revocation. Rejected normal cancellation admission is
recovered by the next timer callback; shutdown never needs a business control
lane or raw inline fallback.

## 5. Metrics and Directional Baseline

The synchronized snapshot includes connection owners; queue typed results and
high-water marks; tick/cadence indices; empty/work ticks; catch-up/skipped ticks;
maximum consecutive catch-up; tick overruns; max commands per tick; handler and
output outcomes; stale drops; cross-domain handoffs; fixed-histogram tick jitter
P50/P99/P999, tick duration P99/P999, queue age P99/P999 and maximum; timer
setup/cancellation outcomes; queued shutdown cancellation and convergence time.

The opt-in benchmark records throughput, these cadence/queue tails, catch-up and
skip behavior, working set, and shutdown for the exact Profile C composition.
It is directional local evidence, not promotion evidence.

## 6. Verification

- `tests/contract/runtime_model/test_dedicated_fixed_tick_profile.cpp` proves
  two network owners, one logic/tick owner, no handler before the authoritative
  tick, bounded 2/2/1 drain, ordered owner return, deterministic bounded
  catch-up then skip, queue saturation/recovery, stale output, and two-future
  shutdown while a handler is active.
- `tests/cmake/test_dedicated_fixed_tick_profile_contract.py` rejects install,
  fixed-delay cadence, event-driven drain/wakeup, missing TimerQueue fixed-rate
  options, unbounded catch-up/work, generation omissions, or direct logic-thread
  transport mutation.
- `examples/runtime_profiles/multi_io_fixed_tick_echo.cpp` is the runnable TCP
  echo composition.
- `benchmarks/runtime_profiles/multi_io_fixed_tick.cpp` is the opt-in
  directional cadence/handoff baseline.

## 7. Non-Goals

- no installed Profile API or change to installed LogicLoop semantics;
- no connection migration, logic sharding, Hybrid dispatch, or business state;
- no fixed-delay compatibility mode, unbounded catch-up, per-command logic wake,
  control-source admission bypass, work stealing, or global executor;
- no UDP, KCP, TLS, HTTP, RPC, coroutine, or experimental transport work.
