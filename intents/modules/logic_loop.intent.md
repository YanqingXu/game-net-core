---
status: active
target: GameNet::game_logic
migration_source: native
promote_gate: none
---

# Module Intent: LogicLoop And GameCommandQueue

## Intent

The game-logic bridge decouples I/O callbacks from game command execution. I/O
threads submit value-type commands into a bounded synchronized queue; one
explicit logic `EventLoop` drains a fixed maximum on each tick.

## Threading And Ownership

- `GameCommandQueue::submit` and snapshot queries are cross-thread safe.
- One `LogicLoop` and its configured `EventLoop` own draining, handlers, output
  callbacks, start, and stop.
- Submit never waits for the logic loop and returns an explicit overload result.
- Handlers and output callbacks run on the logic loop and may re-enter `submit`;
  the queue mutex is not held while callbacks execute.
- `GameCommand` is a value and contains ids, priority, payload, and enqueue time;
  it contains no `TcpConnection`, endpoint, or business object pointer.

## Boundedness

- Queue limits cover command count, aggregate payload bytes, and single payload
  bytes.
- Results distinguish accepted, queue full, payload too large, and stopped.
- Snapshots expose accepted/rejected totals, depth, queued bytes, and high-water
  marks without synchronous logic-loop coordination.

## Verification

`tests/contract/game_logic/test_logic_loop_contract.cpp` verifies concurrent
submit, each rejection reason, fixed tick batch limits, handler/output thread
context, re-entry safety, metrics, and stopped behavior.
