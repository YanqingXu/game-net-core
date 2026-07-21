---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
artifact_kind: installed-library
migration_mode: adapt
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: mini/net/TcpServer.h;mini/net/TcpServer.cc
---

# Module Intent: Graceful Server Shutdown

## 1. Intent

Make server drain an explicit, completion-aware library contract. A caller can
stop admission, drain already-admitted connection output, wait for connection
teardown and worker joins, and receive an explicit result when timeout forces
remaining connections closed.

---

## 2. In Scope

- modules touched: Acceptor, TcpConnection, EventLoopThreadPool, TcpServer
- `TcpServer::stopGracefully()` returns a shared completion future
- shutdown ordering:
  1. stop accepting new connections (Acceptor::stop)
  2. call `TcpConnection::shutdown()` so already-admitted output drains before
     the socket write side closes
  3. wait for normal connection close/removal
  4. on timeout, force-close remaining connections on their owner loops
  5. quit and join worker loops
  6. publish `TcpServerStopResult`
- compatibility `TcpServer::stop()` remains immediate force-close and may
  explicitly escalate an in-progress graceful drain

---

## 3. Non-Responsibilities

- does not install process signal handlers or create a hidden global runtime
- does not mutate loop-owned state off-thread
- does not rely on destructor accidents for cleanup
- does not define process-level SIGINT/SIGTERM policy
- does not wait for application work that was not admitted to TcpConnection
  output before the drain request
- does not change TcpClient shutdown (already sufficient)

---

## 4. Core Invariants

- accept stop precedes final loop teardown
- connection shutdown ordering is explicit (map mutation on base loop, destruction on IO loop)
- pending callbacks do not outlive safe owners (lifetimeToken guards)
- worker loop exit remains coordinated (quit → drain functors → join)
- completion becomes ready only after connection teardown and worker-loop join converge
- repeated graceful-stop calls share the first operation and result
- a negative timeout is rejected before server state changes
- timeout force-close and immediate-stop escalation remain single-shot

---

## 5. Threading Rules

- graceful shutdown coordination and connection-map observation run on base loop
- shutdown requests may originate on any thread and marshal to the base loop
- cross-loop operations:
  - EventLoopThreadPool::stop() calls quit() on each worker loop (cross-thread safe via atomic + wakeup)
  - TcpConnection::shutdown()/forceClose() marshal to the connection owner loop
- the future may be observed from any thread; result publication is synchronized

---

## 6. Failure Semantics

- repeated graceful request returns the same shared future
- timeout result reports the initial and forcibly-closed connection counts
- immediate `stop()` during drain reports `ForcedByImmediateStop`
- a request after an unrelated immediate stop reports `AlreadyStopped`
- scheduling failure and server destruction publish explicit terminal outcomes
- forced close fallback reuses normal owner-loop cancellation/teardown

---

## 7. Test Contracts

- `tests/contract/tcp_server/test_tcp_server_stop_active_write.cpp` verifies:
  - immediate `stop()` retains force-close compatibility during active output
  - a worker-originated graceful request drains all accepted output before EOF
  - repeated calls share one completion result
  - completion reports `Drained` only after normal connection removal
  - a peer that remains open is force-closed after the configured timeout and
    reports the exact forced connection count
  - negative timeouts are rejected before shutdown begins
- existing multi-worker stop contracts continue to verify owner-loop teardown
  and worker join ordering

---

## 8. Review Questions

- Is completion published only after connection teardown and worker joins?
- Can callbacks still target destroyed upper-layer objects?
- Does threaded shutdown converge cleanly?
- Does the timeout force only connections still present at expiry?
- Can repeated or mixed stop requests duplicate close callbacks or completion?
