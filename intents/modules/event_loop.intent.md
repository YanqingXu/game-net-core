---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: EventLoop

## 1. Intent
EventLoop is the core event-dispatch engine bound to one thread.
It waits for active I/O, dispatches Channel callbacks, executes queued tasks,
and serves as the central scheduling point for timer and coroutine extensions.

EventLoop is the heart of reactor execution in game-net-core.

---

## 2. Responsibilities
- own and drive Poller
- perform poll wait loop
- dispatch active Channel events
- execute pending functors submitted from same thread or other threads
- bound pending-functor admission and execute at most a configured batch per
  normal loop iteration so task floods cannot grow memory without limit or
  monopolize the owner thread
- maintain thread-affinity discipline
- provide runInLoop / queueInLoop API
- provide a copyable, non-owning `EventLoopExecutor` that can reject queued work
  after loop admission closes without exposing a dereferenceable raw loop
- support wakeup mechanism for cross-thread responsiveness
- provide safe extension point for timers and coroutine resume
- contain exceptions from asynchronously dispatched Channel, Timer, pending-
  functor, and metric callbacks behind an observable owner-loop policy

---

## 3. Non-Responsibilities
- does not parse business protocols
- does not own all connection objects by itself
- does not define application-level routing logic
- does not replace a thread pool abstraction
- does not directly implement high-level coroutine task graph in v1

---

## 4. Core Invariants
- one EventLoop binds to exactly one thread
- loop() runs only on owner thread
- Poller is used only by owner thread
- pending functors execute on owner thread
- quit must not abandon already-queued pending functors
- wakeup is used to interrupt blocking poll when needed
- channel update/remove must respect EventLoop ownership
- Poller lifetime does not exceed EventLoop lifetime
- EventLoop exclusively owns executor admission state; executor handles neither
  own nor extend EventLoop lifetime
- executor admission closes before the final pending-functor drain so a
  successful `tryQueue` is either in that drain set or a normal loop iteration
- owner-thread identity remains usable only while accepting work or executing
  that final accepted-work drain; it becomes unavailable after the drain ends
- capacity-aware `tryQueueInLoop` and executor submissions never exceed the
  normal pending-functor limit; legacy/control `queueInLoop` may use only the
  separately bounded reserve and throws explicitly when total capacity is full
- normal iterations execute at most `maxFunctorsPerIteration`; accepted
  remainder is preserved and the loop is woken for another I/O/timer-aware
  iteration
- one asynchronous callback exception never skips later ready timers or
  already-accepted pending functors, and Channel dispatch always clears its
  in-callback lifetime guard while unwinding

---

## 5. Collaboration
- owns one Poller
- owns one TimerQueue
- coordinates with Channel objects for I/O event dispatch
- provides scheduling point for timer callbacks and coroutine awaiter resume
- interacts with EventLoopThread / thread-pool wrappers in scaled mode

---

## 6. Main Execution Model
Default v1 loop direction:
1. wait for active I/O via Poller
2. dispatch active channels
3. execute pending functors
4. repeat until quit requested

If quit is observed, EventLoop may stop polling new iterations,
but it should still drain already-queued pending functors before leaving loop().

If this order changes, docs/tests/contracts must be updated.

---

## 7. Public API Direction
Typical API direction:
- loop()
- quit()
- runInLoop(Functor)
- queueInLoop(Functor)
- tryQueueInLoop(Functor) -> bool
- pendingFunctorCount() / rejectedFunctorCount() snapshots
- executor() -> EventLoopExecutor
- setCallbackExceptionHandler(EventLoopCallbackExceptionHandler)
- callbackExceptionCount()
- runAt(Timestamp, Functor)
- runAfter(Duration, Functor)
- runEvery(Duration, Functor)
- cancel(TimerId)
- updateChannel(Channel*)
- removeChannel(Channel*)
- assertInLoopThread()
- isInLoopThread()
- wakeup()

Additional APIs can be added later for richer timer and coroutine features.

---

## 8. Threading Rules
- EventLoop has owner thread identity
- same-thread runInLoop executes immediately
- cross-thread runInLoop behaves like queued scheduling
- queueInLoop enqueues within the normal-plus-reserved hard capacity and throws
  explicitly at saturation
- tryQueueInLoop and EventLoopExecutor return false at normal-capacity saturation
- cross-thread enqueue must ensure wakeup when loop may be blocked
- pending functor queue flush occurs on owner thread only
- cross-thread-observed pending functor execution state is atomic or synchronized

---

## 9. Wakeup Intent
Wakeup exists to solve a specific problem:
the loop may be blocked in poll while another thread submits new work.

Wakeup should:
- be explicit
- be lightweight
- not become a hidden data channel
- not replace normal callback/business delivery semantics

---

## 10. Failure Semantics
EventLoop should explicitly handle:
- backend poll errors/interruption
- wakeup read/write issues
- invalid non-owner-thread mutation attempts
- asynchronous callback exceptions are counted and reported with their source;
  the default action logs and continues, while an installed policy may request
  loop quit without abandoning already-accepted pending functors
- an exception thrown by the exception handler itself is logged and requests
  loop quit rather than recursing through the same handler
- quit request while processing current iteration
- queued functors that were already accepted before loop exit
- executor `tryQueue` after admission close or destruction returns false and
  never dereferences the expired EventLoop
- pending-functor saturation is explicit: capacity-aware APIs return false,
  while the legacy/control API throws `std::overflow_error`

v1 should prefer predictable behavior over over-complicated generic error models.

---

## 11. Future Extension Points
- coroutine awaiter scheduling
- metrics/tracing hooks
- idle strategy tuning
- task priority experimentation

These extensions must preserve EventLoop as the single-thread scheduling core.

---

## 12. Test Contracts
- same-thread runInLoop executes immediately
- cross-thread queueInLoop executes on loop thread
- cross-thread queueInLoop wakes blocked poll
- quit causes safe loop exit
- pending functors preserve expected execution semantics
- bounded admission rejects beyond the configured count without losing any
  already accepted functor
- a per-iteration batch limit yields back to ready timers/I/O before executing
  the remaining accepted batch
- quit still drains already-queued nested functors before loop exit
- executor identity is stable, cross-thread queueing executes on the owner
  thread, and copied handles become unavailable after loop teardown
- work accepted before executor admission closes can still perform owner-only
  operations during the final drain, while new submissions are rejected
- update/remove channel routes through correct Poller interaction path
- `tests/contract/event_loop/test_event_loop.cpp` verifies
  Channel, Timer, pending-functor, and metric callback exceptions are observed,
  later callbacks still run under Continue, Quit still drains accepted work,
  and a throwing thread-init callback is rethrown to `startLoop()` without a
  worker `terminate` or publication deadlock

---

## 13. Review Checklist
- Does this change violate single-owner-thread discipline?
- Is wakeup still correct and necessary?
- Are pending functors executed in a predictable place?
- Does this complicate future timer/coroutine integration?
- Is destruction/shutdown path still safe?
