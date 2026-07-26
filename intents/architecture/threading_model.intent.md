---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Architecture Intent: Threading Model

## 1. Intent
The threading model defines how concurrency is structured in game-net-core.
The goal is to reduce synchronization complexity by concentrating mutable reactor state
inside a single EventLoop thread, while still allowing safe cross-thread task submission.

---

## 2. Core Principle
One EventLoop owns one thread.
That thread is the only place where:
- Poller wait/dispatch occurs
- Channel registration changes occur
- pending functors are flushed
- already-queued pending functors are drained before loop exit
- lifecycle-sensitive state transitions occur

This is the primary concurrency discipline of the system.

Active-batch epoch assignment, slot invalidation, registration generation, and
current-Channel retirement are owner-thread-only and need no cross-thread lock.

---

## 3. Why This Model Exists
This model is chosen to:
- reduce lock complexity
- keep event ordering understandable
- keep callback execution context predictable
- simplify future coroutine resume semantics
- align with mature reactor framework design

---

## 4. Owner Thread Rule
Each EventLoop records its owner thread identity.
The following operations are owner-thread only:
- loop()
- update/remove channel registration
- processing active channels
- flushing pending functors
- loop-owned timer dispatch
- connection state mutation under that loop

Violations should be guarded, asserted, or explicitly rejected depending on severity.

---

## 5. Cross-Thread Interaction Model
Cross-thread callers must not directly mutate loop-owned state.

Approved paths:
- runInLoop(fn)
- queueInLoop(fn)
- wakeup()
- a pre-registered EventLoopControlSource for bounded internal lifecycle work

### runInLoop(fn)
- executes immediately if caller is already in owner thread
- otherwise enqueues fn and ensures wakeup if necessary

### queueInLoop(fn)
- always enqueues
- loop thread executes queued functions later
- if fn was accepted before quit completes, it should still run on the owner loop thread

### wakeup()
- interrupts blocked poll wait
- causes loop to observe pending work sooner

### EventLoopControlSource::notify()
- may be called from any thread after owner-thread registration
- receives its registration only through the non-installed source-private
  control registry; ordinary public callers cannot register control work
- sets one preallocated mailbox bit; repeated notifications coalesce
- never consumes normal or reserved pending-functor capacity
- executes its registered callback only on the EventLoop owner thread
- is reserved for finite internal control/lifecycle state transitions rather
  than user or business work
- returns an explicit PostResult so quit and expired-owner races are observable

---

## 6. Dispatch Ordering
Within one loop iteration, the system should have a clear order for:
- poll wait
- active channel dispatch
- queued functor flush
- timer-related work if present

Exact micro-order can evolve, but must remain documented and stable enough for reasoning.

v1 default direction:
1. poll for active events
2. assign the active-batch epoch and dispatch still-valid unique Channel slots
3. dispatch expired timers
4. execute one control-source round
5. execute pending functors
6. repeat

If quit is requested during an iteration, already-queued functors should still be drained
before the loop fully exits.

Quit also linearizes control admission. A control notification accepted before
the Accepting-to-Draining transition is executed before loop exit. External
notifications after that transition return Shutdown; only the currently
executing control callback may re-arm its own source during the final
accepted-work drain.

If this changes, related contracts/tests/docs must be updated.

---

## 7. Callback Thread Context
Unless explicitly documented otherwise:
- Channel callbacks run in EventLoop owner thread
- queued tasks run in EventLoop owner thread
- connection callbacks run in owning EventLoop thread
- write/read/error/close handling runs in owning EventLoop thread

The system should avoid ambiguous execution contexts.

---

## 8. Wakeup Design Intent
Wakeup is not business messaging.
Wakeup is a scheduling interrupt used to:
- break blocking poll
- accelerate queued task handling
- maintain responsiveness for cross-thread submissions

Wakeup must remain lightweight and explicit.

---

## 9. Scaling Model
The threading model supports:
- single-loop single-thread v1 deployment
- one-loop-per-thread scaling in thread pool model
- accept loop handing off connections to worker loops

The model does not assume shared mutable connection state across loops.

---

## 10. Coroutine Compatibility
Future coroutine integration must respect this model.
Await suspension/resume should not invent a new concurrency model.
Resume should return to the appropriate EventLoop thread unless explicitly designed otherwise.

---

## 11. Failure / Risk Areas
High-risk threading mistakes:
- direct Poller mutation from other thread
- callback execution on wrong thread
- forgetting wakeup after cross-thread queue
- hidden shared state between loops
- destruction path racing with queued callbacks

These must be explicitly guarded in implementation and tests.

---

## 12. Test Contracts
- same-thread runInLoop is immediate
- cross-thread runInLoop is marshaled correctly
- queueInLoop from another thread wakes loop
- queued work executes on loop thread
- normal and reserved queue saturation does not reject a registered control source
- repeated notifications for one control source coalesce without recursion
- quit races produce either an executed Accepted request or an explicit
  Shutdown/OwnerUnavailable result
- channel registration change is loop-thread enforced
- removing one active Channel invalidates its O(1) batch slot before another
  owner-thread callback may release it

---

## 13. Review Questions
- Does this change create a non-owner-thread mutation path?
- Is callback execution context still predictable?
- Is wakeup still correctly used?
- Does this make future coroutine scheduling harder or easier?
