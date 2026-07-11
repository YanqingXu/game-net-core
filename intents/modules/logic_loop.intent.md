---
status: active
target: GameNet::game_logic
migration_source: mini_trantor
promote_gate: none
artifact_kind: installed-library
migration_mode: redesign
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: mini/game/logic/GameCommandQueue.h;mini/game/logic/GameCommandQueue.cc;mini/game/logic/LogicLoop.h;mini/game/logic/LogicLoop.cc
---

# Module Intent: LogicLoop And GameCommandQueue

## Intent

The game-logic bridge decouples I/O callbacks from game command execution. I/O
threads submit value-type commands into a bounded synchronized queue; one
explicit logic `EventLoop` drains a fixed maximum on each tick.

## Threading And Ownership

- `GameCommandQueue::submit` and snapshot queries are cross-thread safe.
- One `LogicLoop` and its configured `EventLoop` own draining, handlers, output
  callbacks, start, and stop. The caller owns both objects and must keep the
  owner EventLoop alive and accepting until a Running LogicLoop has been stopped
  or destroyed on that owner. Destroying a still-Running LogicLoop after its
  owner becomes unavailable is a contract violation and fails fast rather than
  dereferencing a stale loop pointer.
- Lifecycle is one-shot: `Created -> Running -> Stopping -> Stopped`.
  Duplicate start while Running is idempotent; start after Stopped is rejected.
- `state()` / `running()` are atomic cross-thread snapshots. They do not allow
  non-owner mutation of lifecycle or callbacks.
- Submit never waits for the logic loop and returns an explicit overload result.
- Handlers and output callbacks run on the logic loop and may re-enter `submit`;
  the queue mutex is not held while callbacks execute.
- Metric callbacks also run on the logic loop and may re-enter `submit` or
  lifecycle observers without a queue or callback lock being held.
- Handler, output, and metric callbacks may release the caller-owned
  `LogicLoop` on its owner loop. Callback invocation uses a local callable
  snapshot; destruction revokes execution permission independently of token
  ownership, and the active tick returns without accessing another member or
  invoking a later callback. Calling `stop()` without destruction retains the
  committed-current-batch behavior below.
- Repeating timer work captures shared lifetime state whose atomic permission is
  revoked before object teardown. A strong state reference cannot extend
  execution permission after destruction.
- `GameCommand` is a value and contains ids, priority, payload, and enqueue time;
  it contains no `TcpConnection`, endpoint, or business object pointer.

## Boundedness

- Queue limits cover command count, aggregate payload bytes, and single payload
  bytes.
- Results distinguish accepted, queue full, payload too large, and stopped.
- Snapshots expose accepted/rejected totals, depth, queued bytes, and high-water
  marks without synchronous logic-loop coordination.
- `stop()` closes admission, cancels the tick timer, discards queued commands,
  and returns command/byte drop counts. Discarded work is never handled later.
- Commands removed by `drain()` form one committed current batch. If a handler
  or output callback calls `stop()`, the remaining commands in that current
  batch still run and the tick metric is emitted; only commands still queued
  behind the batch are discarded. Reentrant submit after stop returns `Stopped`.

## Verification

- `tests/contract/game_logic/test_logic_loop_contract.cpp` verifies rejection
  reasons, fixed tick batch limits, handler/output context, metrics, and stopped
  behavior.
- `tests/contract/game_logic/test_logic_loop_concurrency.cpp` uses explicit
  latches plus test-build-only internal queue hooks to prove a producer enters
  `submit()` while `drain()` holds the queue mutex. It separately proves four
  producers submit while the logic handler is active, verifies unique ids
  without duplicate or unaccounted work, covers handler/output/metric callback
  re-entry, and locks the callback-stop committed-current-batch semantics. The
  queue hooks are absent when `GAMENET_BUILD_TESTING=OFF` and add no installed
  public API.
- `tests/contract/game_logic/test_logic_loop_lifecycle.cpp` verifies one-shot
  transitions, missing-handler rejection, duplicate start, restart rejection,
  backlog discard/drop accounting, destruction with a pending tick timer, and
  deterministic owner release from handler/output/metric callbacks.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: value commands, bounded synchronized admission, one logic
  owner loop, fixed-tick bounded drain, and callback execution off the queue lock.
- Deferred from the source design: adaptive policy and business simulation stay
  outside the active logic bridge.
- Dropped behavior: none; atomic state observation, revocable timer/callback
  lifetime protection, real producer/loop overlap, and callback re-entry are
  restored by hardening.
