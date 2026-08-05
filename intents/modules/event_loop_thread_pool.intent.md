---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: EventLoopThreadPool

## 1. Intent
EventLoopThreadPool scales one-loop-per-thread execution by managing a set of
EventLoopThread workers and returning loops for connection assignment.

---

## 2. Responsibilities
- start configured worker EventLoopThread instances
- expose base loop when zero worker threads are configured
- select worker loops with an explicitly configured round-robin,
  least-connections, pending-functor queue-lag, or consistent-hash policy
- retain exact base-loop-owned active-connection assignment counts for
  least-connections selection without owning the connections themselves
- use stable worker indices and deterministic rendezvous hashing for affinity
  keys; the same key and worker set select the same loop
- stop all worker loops on explicit request (stop())
- serve as the final join barrier before TcpServer publishes graceful-stop completion

---

## 3. Non-Responsibilities
- does not own connection lifecycle directly
- does not share mutable connection state across loops
- does not replace EventLoop scheduling semantics
- does not claim queue-lag includes kernel completions, active I/O, timers, or
  callback execution time; it observes only pending-functor queue age/count

---

## 4. Core Invariants
- base loop remains the fallback loop when thread count is zero
- configured thread count is non-negative and remains immutable from a
  successful start until stop completes
- the control state machine is `Idle -> Started -> Idle`; repeated start while
  Started is rejected without publishing another worker or resetting load
  accounting, while stop in Idle is an idempotent no-op
- startup failure returns the pool to Idle with no published workers, selection
  metadata, or connection-load observations, so a later start may retry
- loop selection is deterministic and bounded by started workers
- thread-pool control remains on the base loop thread
- selection policy is immutable after start
- least-connections counts change exactly once when TcpServer commits or
  releases a base-map connection assignment
- equal least-connection and queue-lag scores rotate their first candidate so
  persistent ties do not pin every assignment to worker zero
- consistent-hash requires a non-empty affinity key when workers exist

---

## 5. Threading Rules
- thread-count and policy configuration, start(), stop(), selection, loop-list
  observation, and connection-load accounting are base-loop-thread operations
- an off-owner control call fails before reading or mutating pool state
- worker loops are only used through EventLoop scheduling APIs after publication

---

## 6. Failure Semantics
- a negative thread count fails with `std::invalid_argument` before changing the
  configured count
- thread-count or policy mutation while Started and repeated start fail with
  `std::logic_error` before changing worker, selector, or load state
- repeated selection must not step outside the worker loop array
- unknown policies, empty consistent-hash keys, foreign loop accounting,
  connection-count underflow, and policy mutation after start fail explicitly
- zero-thread start invokes the initialization callback exactly once on the
  base loop and keeps selection on that loop
- partial worker startup failure stops and joins already-published workers,
  clears loop selection state, returns to Idle, and rethrows the initialization
  exception

---

## 7. Test Contracts
- negative thread counts, wrong-thread configuration, late configuration, and
  repeated start are rejected before observable state changes
- zero-thread start keeps work on base loop
- multi-thread start publishes the configured worker loops
- getNextLoop rotates through workers predictably
- least-connections selects the smallest recorded load and releases load exactly
- queue-lag prefers an empty queue, then the newest oldest-pending timestamp,
  with pending count and rotating worker order as deterministic tie breakers
- consistent-hash keeps repeated keys on one worker and distributes a key set
  without depending on pointer values or implementation-defined `std::hash`
- stop() quits all worker loops and clears thread/loop containers
- stop followed by start republishes exactly the configured worker count;
  configuration may change only while the pool is Idle
- TcpServer forwards the same negative and post-start thread-count rejection
- cross-thread queued work reaches each published worker loop under a light
  soak and does not run on the base loop when workers are configured
- `tests/contract/event_loop_thread_pool/test_event_loop_thread_pool_restart_soak.cpp`
  verifies repeated start/stop cycles publish worker loops, execute queued work
  on workers, and return selection to base-loop fallback after stop()

---

## 8. Review Checklist
- Is base-loop ownership still clear?
- Is loop selection deterministic?
- Are all selector inputs owner-thread snapshots rather than cross-thread
  mutable connection state?
- Does every committed TcpServer assignment have one matching load release?
- Does startup preserve one-loop-per-thread discipline?
