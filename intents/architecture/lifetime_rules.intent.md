---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Architecture Intent: Lifetime Rules

## 1. Intent
The lifetime rules define how object destruction, callback safety, registration consistency,
and cross-layer references are managed in game-net-core.

The goal is to prevent the most common reactor bugs:
- callback after object destruction
- stale registration in Poller
- use-after-free through raw callback capture
- hidden ownership cycles
- destruction on wrong thread

---

## 2. Core Principle
Lifecycle-sensitive objects must have explicit destruction discipline.

The project does not rely on “probably still alive” assumptions.
It requires:
- explicit ownership
- explicit observation
- explicit registration removal
- explicit callback safety strategy

---

## 3. Key Lifecycle-Sensitive Objects
- EventLoop
- Poller
- Channel
- Acceptor
- TcpConnection
- TimerQueue
- future coroutine awaiters bound to loop-owned resources

---

## 4. Registration Rule
If an object is registered into a backend event system,
its destruction path must ensure registration is removed before effective invalidation.

Typical rule:
remove before destroy.

For Channel, successful remove is also the active-batch invalidation
linearization point. EventLoop must make every not-yet-dispatched entry for that
registration unreachable before remove returns. Re-registration creates a new
generation and cannot revive readiness captured for the removed generation.

This is especially important for:
- Channel vs Poller
- future timers vs timer container
- connection-owned channel teardown

---

## 5. Callback Safety Rule
When lower-layer callbacks may indirectly target upper-layer objects,
the system must guard against expired owners.

The primary strategy in reactor-style callback safety is:
- tie/weak observation
- explicit shared owner lock attempt before dangerous callback path

This is preferred over blind raw pointer callback assumptions.

---

## 6. EventLoop / Poller Lifetime
- EventLoop owns Poller
- Poller must not outlive EventLoop
- Poller must not keep stale references to destroyed Channel
- EventLoop shutdown path must avoid processing invalid backend registrations
- EventLoop owns registered control callbacks and their fixed mailbox slots
- EventLoopControlSource handles are non-owning capabilities and never extend
  EventLoop lifetime
- the source-private EventLoopControlRegistry is the only registration access
  path and is not part of the installed public scheduling surface
- a subsystem that ends before its EventLoop must unregister its control source
  on the owner thread after stopping producers
- unregister invalidates every copied handle generation and clears its pending
  mailbox bit before callback storage is released

---

## 7. Channel Lifetime
- Channel belongs to one EventLoop
- Channel does not own EventLoop
- Channel does not own fd by default
- Channel may outlive or underlive external fd owner only if registration semantics remain valid
- tied owner expiration must block unsafe callback dispatch path
- `tie` protects the owner only after the Channel callback starts; EventLoop
  active-batch membership protects a different Channel destroyed before its
  callback starts
- an internal owner releasing the currently executing Channel transfers that
  removed Channel into the source-private current-dispatch retirement slot
  until `handleEvent` returns; it must not rely on a pending-functor queue

---

## 8. TcpConnection Lifetime Direction
TcpConnection is a high-risk lifecycle object.

Its lifetime design should ensure:
- callbacks do not run on already-destroyed application-side state
- close path is unified
- repeated close/error handling does not produce inconsistent callback behavior
- destruction is coordinated with loop-thread registration removal

A connection should not disappear while loop still believes it is actively registered.

---

## 9. Destruction Thread Intent
Where destruction affects loop-owned state,
the destruction path should respect owner-thread assumptions.

If removal from Poller or loop-owned structures is needed,
destruction should be coordinated through loop thread rather than arbitrary external thread.

---

## 10. Forbidden Lifetime Patterns
- destroying a registered channel without unregistering it
- capturing raw this in delayed callback without lifetime guarantee
- letting Poller own Channel
- mixing ownership and observation semantics without documentation
- relying on destructor side effects to fix broken registration discipline

---

## 11. Test Contracts
- remove-before-destroy path is enforced
- tied owner expiration blocks unsafe callback path
- repeated teardown path is idempotent or guarded
- registration container remains consistent after object removal
- queued callback does not use invalidated owner object
- copied control-source handles return OwnerUnavailable after unregister or
  EventLoop destruction and never dereference expired loop storage
- stale active-batch entries are invalidated before Channel destruction, and a
  same-address re-registration cannot pass the old generation check
- repeated remove cannot erase a same-fd replacement registration

---

## 12. Review Questions
- Who owns this object?
- Who only observes it?
- Can callback outlive owner?
- Is registration removed before destruction?
- Is destruction occurring in a safe thread context?
