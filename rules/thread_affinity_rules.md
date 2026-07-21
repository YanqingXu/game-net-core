# thread_affinity_rules.md

## 1. Core Principle
game-net-core follows a reactor-style thread-affinity model.
Mutable reactor state belongs to a specific EventLoop thread.

## 2. EventLoop
- Each EventLoop has exactly one owner thread
- loop() must run only on the owner thread
- Poller operations must run on the owner thread
- pending functors are executed on the owner thread

## 3. Allowed Cross-Thread Interaction
Cross-thread interaction must go through:
- runInLoop(...)
- queueInLoop(...)
- wakeup mechanism

No other direct mutation path is allowed for core loop state.

## 4. runInLoop
- If current thread == owner thread: execute immediately
- Else: enqueue and wakeup loop if needed

## 5. queueInLoop
- Enqueue only within the configured normal-plus-reserved hard capacity;
  saturation is explicit and never silently drops accepted work
- Capacity-aware callers use `tryQueueInLoop()` / `EventLoopExecutor::tryQueue()`
  and handle a false result
- Must ensure wakeup when loop may be blocked in poll

## 6. Channel
- Channel update/remove must occur on its owning EventLoop thread
- handleEvent executes in EventLoop thread unless explicitly documented otherwise

## 7. TcpConnection
- State transitions occur on owning EventLoop thread
- `connected()` and `disconnected()` are cross-thread-safe snapshot observers;
  no other loop-owned state becomes cross-thread-readable through them
- Actual socket write/read handling occurs on owning EventLoop thread
- Cross-thread send request must be marshaled back into loop thread
- Socket-option mutation, context access, callback replacement, connection
  establishment, and connection destruction are owning-EventLoop-thread only

## 8. Logger
- Logger is process-global and is not owned by an EventLoop
- Log emission and configuration may be called from any thread
- Callback configuration is snapshotted under Logger-internal synchronization
- Output callbacks run without the configuration lock and may be concurrent
- Callback-owned sinks and captured mutable state require their own synchronization
- `resetForTesting()` requires a quiescent test boundary

## 9. TcpServer Shutdown
- Graceful-stop requests may originate on any thread
- Acceptor state, connection-map state, drain timers, and terminal decisions
  are mutated only on the server base loop
- Per-connection shutdown and force-close remain marshaled to each connection's
  owner loop
- Completion is published only after worker-loop join has converged

## 10. Listener Error Policy
- Acceptor and TcpServer accept-error policy callbacks execute on the base /
  Acceptor owner EventLoop
- Runtime accept retry timers and Channel read-interest changes remain confined
  to that same owner loop
- A policy callback may choose Retry or Stop; neither action permits direct
  worker-loop mutation

## 11. Callback Exceptions
- EventLoop exception handlers execute on the affected EventLoop owner thread
- TcpConnection exception observers execute on that connection's owner loop
- Asynchronous callback exceptions never migrate cleanup to another thread;
  Continue/Quit and connection close actions reuse normal owner-loop paths
- Thread-init exceptions cross back to the creator only through the synchronized
  EventLoopThread startup handshake

## 12. Wakeup
- Wakeup write can be invoked cross-thread
- Wakeup read/clear is handled in loop thread
- Wakeup is a scheduling signal, not business event delivery

## 13. TcpServer Admission
- Global/per-peer active counts, per-peer rate buckets, peer-table expiry, and
  unauthenticated timers belong to the server base EventLoop
- Worker-loop authentication completion is a request marshaled through the
  base-loop executor; base-loop execution order resolves deadline races
- Admission metric callbacks execute on the base loop and may not move socket
  or connection lifecycle work onto an arbitrary caller thread

## 14. Metrics Export
- MetricsExporter has no owner EventLoop and may receive concurrent calls from
  multiple producer loops
- recorder callbacks execute synchronously on the original hook thread and do
  not move producer work to another thread
- InMemoryMetricsExporter synchronizes only its own aggregate maps; it never
  mutates reactor or game state
- snapshot rendering and any future blocking export I/O must run outside hot
  owner-loop callbacks
- recorder adapters contain exporter exceptions at the observability boundary

## 15. Forbidden
- Direct Poller mutation from non-owner thread
- Direct Channel mutation that changes registration from non-owner thread
- User callback execution in ambiguous thread context
- “Occasionally safe” thread behavior without rule support
