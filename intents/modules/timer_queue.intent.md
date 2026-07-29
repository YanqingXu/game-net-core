---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: TimerQueue

## 1. Intent
TimerQueue provides owner-loop timer scheduling for one EventLoop.
It lets EventLoop compute the next poll timeout, then translates elapsed
deadlines into ordered timer callback dispatch while preserving reactor
thread-affinity and lifecycle discipline.
The same module also provides DeadlineQueue, a callback-free bucketed index for
large homogeneous connection/session deadline populations that are driven by
one owner-loop timer or an explicit owner-loop sweep.

---

## 2. Responsibilities
- store one-shot and repeating timer metadata
- provide the nearest timeout to EventLoop before each backend poll
- trigger ready timers on the owner EventLoop thread
- extract at most EventLoop's validated expired-timer budget per loop turn;
  later ready timers remain in the ordered container and keep the next poll
  timeout at zero
- reschedule repeating timers with explicit fixed-delay or fixed-rate cadence
- bound fixed-rate replay with a per-timer maximum consecutive catch-up count;
  once exhausted, skip missed cadence points and schedule the next future point
- support cross-thread add/cancel through EventLoop scheduling APIs
- quantize large logical deadline populations into configured-resolution
  buckets instead of allocating one TimerQueue callback per target
- schedule/reschedule one active deadline per numeric key with generation-safe
  cancellation and a configured expiration budget per advance

---

## 3. Non-Responsibilities
- does not invent a separate scheduler outside EventLoop
- does not own arbitrary user objects captured by timer callbacks
- does not provide business retry / timeout policy by itself
- does not bypass EventLoop quit and teardown semantics
- DeadlineQueue does not invoke callbacks, own connections/sessions, or create
  its own driver timer

---

## 4. Core Invariants
- one TimerQueue belongs to exactly one EventLoop
- timer container mutation happens only on the owner loop thread
- TimerQueue owns no backend fd; EventLoop owns the poller wakeup mechanism
- ready timer callbacks run on the owner loop thread
- repeating timers are only reinserted if they were not canceled
- the compatibility `runEvery(Duration, Functor)` overload is fixed-delay:
  callback completion time plus interval determines the next expiration
- fixed-rate timers derive the next expiration from the prior scheduled point;
  they execute at most the configured consecutive catch-up count, then skip to
  the next future cadence point
- catch-up callbacks are ordinary budgeted timer callbacks on later loop turns,
  never recursive calls from the current callback frame
- cancel must prevent future firing, even if requested cross-thread
- a callback may cancel a later ready timer that was not extracted into the
  current budget; removal from the ordered container prevents its later firing
- one DeadlineQueue belongs to one owner EventLoop; every operation is
  owner-loop-only
- a DeadlineKey has at most one active entry; reschedule publishes a new
  generation, and stale tokens cannot cancel its replacement
- deadline quantization rounds up, so a bucket never expires a target before
  its requested deadline and lateness is bounded by one resolution interval
- advance visits only ready non-empty buckets and extracts at most the
  configured budget; future populations are not scanned

---

## 5. Collaboration
- owned by EventLoop
- EventLoop calls pollTimeoutMs(defaultTimeoutMs) before Poller::poll()
- EventLoop calls handleExpired(now, maxCount) after active I/O dispatch
- relies on EventLoop runInLoop / queueInLoop for cross-thread add/cancel
- may later provide scheduling substrate for connection idle timeout or delayed shutdown features
- TcpServer authentication deadlines and SessionManager idle deadlines use
  DeadlineQueue while retaining their own target ownership and close policy

---

## 6. Threading Rules
- addTimerInLoop / cancelInLoop are owner-thread-only operations
- public timer APIs on EventLoop may be called cross-thread, but must marshal into the owner loop
- timer callbacks execute in the owner loop thread only
- timeout calculation and expired timer dispatch happen in the owner loop thread only
- DeadlineQueue schedule/cancel/advance/inspection are owner-loop-only

---

## 7. Ownership Rules
- EventLoop owns TimerQueue
- TimerQueue owns timer metadata entries
- DeadlineQueue owns bucket/index metadata only
- Timer callbacks borrow any captured objects; they must not imply hidden ownership transfer

---

## 8. Failure Semantics
- invalid timer intervals fail explicitly rather than silently corrupt scheduling
- cancel of an already-fired or unknown timer is a safe no-op
- quit during timer callback processing must not abandon already-accepted owner-loop work
- TimerQueue destruction must tolerate an empty or partially canceled timer set
- invalid DeadlineQueue resolution/budget fail explicitly; stale-token cancel
  is a safe no-op

---

## 9. Public API Direction
Exposed through EventLoop:
- runAt(Timestamp, Functor)
- runAfter(Duration, Functor)
- runEvery(Duration, Functor)
- runEvery(Duration, Functor, RepeatingTimerOptions)
- cancel(TimerId)
- DeadlineQueue::schedule/cancel/advance/clear/size/nextBucketDeadline

`TimerId` exists to make cancellation explicit and avoid exposing internal timer pointers.
`RepeatingTimerOptions` defaults to fixed-delay with zero catch-up so the
existing overload keeps its historical behavior.

---

## 10. Test Contracts
- one-shot timer fires on the owner loop thread
- repeating timer fires multiple times and can be canceled
- legacy fixed-delay waits one interval after a late callback completes
- fixed-rate preserves scheduled cadence, performs exactly the configured
  maximum catch-up callbacks, and then skips missed points
- fixed-delay rejects a non-zero catch-up setting instead of ignoring it
- cross-thread runAfter marshals back and fires on the owner loop thread
- cancel before expiration prevents callback execution
- canceling a ready timer from an earlier ready callback prevents the canceled
  callback from firing
- a ready population larger than the configured per-iteration budget retains
  order, yields to later EventLoop phases, and reports exact remaining work
- timer queue teardown owns only timer metadata and does not leave backend registration behind
- bucket quantization never fires early; same-key replacement rejects a stale
  cancel; ready populations larger than the advance budget retain exact
  continuation state
- a large future population plus a small ready population advances only the
  ready buckets, and cross-thread mutation is rejected

---

## 11. Review Checklist
- Is timer state still fully owner-loop-owned?
- Does EventLoop still call pollTimeoutMs() before blocking in Poller?
- Can cancel race with callback execution without double-fire?
- Do timer callbacks still obey EventLoop thread-affinity rules?
- Does this design compose with future async timeout APIs without bypassing EventLoop?
