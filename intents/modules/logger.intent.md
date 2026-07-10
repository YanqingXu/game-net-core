---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: Logger

## 1. Intent
Logger is synchronous process-global base infrastructure for diagnostic records.
It is independent of EventLoop ownership and provides safe concurrent emission
and runtime configuration without creating another reactor thread domain.

---

## 2. Responsibilities
- format each record on the calling thread
- expose a process-global atomic log-level threshold
- store copies of normal-output, topic-output, and fatal-flush callbacks
- permit callback replacement while other threads are emitting records
- snapshot callback configuration before dispatching one completed record
- preserve the existing `LOG_*`, coost-style, topic, and `CHECK*` interfaces

---

## 3. Non-Responsibilities
- does not own or marshal through an EventLoop
- does not provide an asynchronous logging worker or file rotation
- does not serialize independent callback invocations from different threads
- does not own state captured by user-provided callbacks
- does not make recursive logging through the same callback bounded

---

## 4. Core Invariants
- `setLogLevel()`, callback setters, `logLevel()`, and record emission may run
  concurrently from arbitrary threads without racing on Logger-owned state
- a record uses one callback snapshot taken when its destructor emits the record
- runtime callback replacement affects later snapshots; an in-flight snapshot
  may continue invoking the previously installed callback copy
- callback and flush invocation never holds the Logger configuration mutex
- output callbacks may run concurrently and must synchronize their own sinks and
  captured mutable state
- `resetForTesting()` is only for a quiescent test boundary with no concurrent
  logging or configuration

---

## 5. Threading Rules
- no EventLoop owns Logger
- record formatting stays on the caller thread
- log-level reads/writes use atomic point-in-time observation
- callback installation and snapshot copying use Logger-internal synchronization
- output, topic-output, and flush callbacks execute on the emitting caller thread
- callbacks may re-enter Logger configuration without deadlocking
- a callback that logs through itself is responsible for avoiding unbounded recursion

---

## 6. Ownership Rules
- process-global Logger configuration owns copies of installed `std::function` objects
- the installer owns the lifetime discipline of state captured by those functions
- an old callback capture may remain alive until every in-flight snapshot releases it
- each temporary Logger record owns its formatted buffer until emission completes

---

## 7. Failure Semantics
- suppressed records do not evaluate streamed expressions
- `FATAL` and failed `CHECK*` records output, flush the same snapshot, and abort
- empty output/flush callbacks fall back to the default stderr implementation
- callback exceptions are not caught by the reactor core and must not be used as
  a cross-thread error transport

---

## 8. Test Contracts
- `tests/unit/base/test_logger.cpp` verifies formatting, filtering, macro gating,
  topic routing, and reset behavior
- `tests/contract/base/test_logger_contract.cpp` verifies fatal child-process and
  system-error output contracts
- `tests/contract/base/test_logger_thread_safety.cpp` verifies concurrent emission,
  log-level/callback replacement, exact single-dispatch accounting, and callback
  re-entry into configuration
- the Logger thread-contract guard verifies active intent, public documentation,
  synchronization primitives, TSan labeling, and CI/long-soak registration

---

## 9. Review Checklist
- Is any Logger-owned global state accessed without atomic or mutex protection?
- Is user code invoked only after the configuration mutex is released?
- Does the change accidentally promise serialized callback invocation?
- Are callback capture lifetime and runtime replacement semantics explicit?
- Does the threading contract test run in the TSan and long-soak slices?
