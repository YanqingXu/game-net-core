---
status: active
target: GameNet::core
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
---

# Use-Case Intent: Production Fault Injection and Endurance

## 1. Intent
Exercise recoverable failures repeatedly inside one long-lived process and
retain machine-verifiable evidence that the Reactor/TCP foundation preserves
thread affinity, bounded resource admission, failure isolation, and teardown
convergence for the full candidate and release durations.

## 2. Fault Profiles
- abrupt peer reset after admission
- throwing message callback with connection-local isolation
- output admission beyond the configured hard limit
- a healthy request after prior failures to prove continued service
- a slow-reader connection forced through bounded graceful-stop timeout

Every completed cycle must exercise every profile exactly once. A shortened
smoke run may validate orchestration but is not release evidence.

## 3. Ownership and Threading
- the endurance executable owns all client sockets and releases them within the
  cycle that created them
- TcpServer owns accepted sockets, connection bookkeeping, admission state, and
  graceful-stop state exactly as defined by the existing Core intents
- connection and callback mutations execute on their owner EventLoop; the
  client driver may only request public cross-thread-safe operations
- callback failure, reset, overload, and timeout reuse the normal
  close/remove-before-destroy lifecycle paths
- the Python supervisor owns only the child process, captured log, checkpoints,
  and final evidence; it never reaches into reactor state

## 4. Evidence Contract
- `candidate-24h` is fixed at 86,400 seconds and `release-72h` at 259,200
  seconds; production modes reject duration overrides
- both modes run one uninterrupted fault-injection executable process and bind
  its executable hash, immutable candidate SHA, platform, backend, start/end
  timestamps, heartbeat sequence, completed cycles, and final result
- Linux production evidence samples the same child process RSS after every
  heartbeat before acknowledging that heartbeat back to the still-live child,
  and enforces reviewed 512 MiB maximum / 64 MiB growth budgets
- heartbeats are monotonic, bounded in silence, and report cumulative counts
  for every declared fault profile
- the release gate accepts 72-hour evidence only for the same frozen commit
  that already passed the 24-hour gate
- GitHub-hosted runners are not eligible because their maximum job execution
  time is shorter than either production duration; the workflow targets a
  dedicated self-hosted endurance runner

## 5. Failure Semantics
- any child exit, malformed or missing heartbeat, non-monotonic counter,
  profile omission, observation-acknowledgment failure, liveness timeout, or
  duration shortfall fails closed
- a failure still writes a checkpoint and final structured result for diagnosis
- runner cancellation or host loss cannot be reported as a passing result
- restarting the child or combining shorter shards cannot satisfy a production
  duration

## 6. Verification
- `tests/integration/resilience/test_fault_injection.cpp` verifies every fault
  profile, owner-loop callbacks, service recovery, overload rejection, and
  forced graceful-stop convergence in one reusable cycle
- `tests/ci/test_endurance_gate.py` verifies smoke execution, fixed production
  durations, workflow/runner wiring, structured checkpoint evidence, and
  malformed/failed-child negative fixtures

## 7. Review Checklist
- Does one process remain alive for the complete claimed duration?
- Does every cycle exercise all five fault profiles?
- Can any driver operation mutate loop-owned state off-thread?
- Are all sockets and callbacks released through existing lifecycle paths?
- Does evidence fail closed on missing duration, identity, or heartbeat data?
