---
status: active
target: GameNet::core
migration_source: native
promote_gate: none
---

# Use-Case Intent: Core Performance Baseline

## 1. Intent
The core performance baseline is a reproducible, opt-in engineering tool for
measuring the current Reactor/TCP implementation on Linux epoll and Windows
IOCP. It records comparable evidence without turning timing thresholds into
correctness tests or expanding the installed `GameNet::core` API.

---

## 2. Responsibilities
- build only when `GAMENET_BUILD_BENCHMARKS=ON`
- exercise the public TcpServer/TcpConnection path over loopback TCP
- measure echo round-trip latency and application-byte throughput
- measure process working-set growth while holding idle connections
- observe working-set growth and high-water notifications for slow-reading clients
- report connection count, EventLoop worker count, backend, completion mode,
  build type, parameters, and measurements as one versioned JSON document
- provide a manual-only workflow that captures the same fixed Release scenario
  set as raw Linux and Windows artifacts from one commit
- return a non-zero exit code when setup, I/O, timeout, or schema production fails

---

## 3. Non-Responsibilities
- is not a CTest and defines no pass/fail performance threshold
- does not claim cross-machine scores are directly comparable
- does not install a benchmark library or add public headers
- does not implement a backpressure policy, memory cap, metrics subsystem, or
  alternate IOCP completion-drain strategy
- does not benchmark HTTP, protocol framing, TLS, UDP, KCP, or game-server layers

---

## 4. Scenario Contracts

### `echo`
- establish the requested loopback connections before the timed interval
- run sequential request/echo round trips per connection from blocking client workers
- record every completed RTT sample and report P50/P99 in microseconds
- define throughput as request bytes plus echoed response bytes divided by elapsed time

### `connections`
- sample process working set after server startup and before client creation
- hold the requested accepted connections through a configurable settle interval
- report before/after/delta working set and approximate delta per connection

### `slow-client`
- clients connect with a deliberately small receive buffer and do not read
- each accepted connection is offered the configured payload from its owner loop
- report offered bytes, high-water callback count, and working-set change after settling
- describe the result as observation of the current notification-only behavior,
  not proof of a bounded output policy

---

## 5. Output Contract
- stdout contains exactly one JSON document with schema
  `gamenet.core_benchmark.v1`; diagnostics use stderr
- every scenario reports `platform`, `backend`, `completion_mode`, and `build_type`
- every scenario reports configured `connections` and `event_loop_threads`
- measurement keys remain present across scenarios; values that do not apply are `null`
- Windows reports the current single-completion
  `GetQueuedCompletionStatus` mode; Linux reports the batched `epoll_wait` mode
- raw JSON output is the durable comparison artifact; documentation summaries
  must identify platform, build type, command, and date

---

## 6. Threading Rules
- the process main thread owns and destroys EventLoop and TcpServer
- TcpServer callbacks execute only on their assigned connection owner loops
- each blocking benchmark client socket is owned by exactly one driver thread
- benchmark coordination uses atomics, a condition variable, and isolated result storage
- no benchmark worker directly mutates loop-owned server or connection state
- server stop and final connection-map inspection execute on the base EventLoop thread

---

## 7. Ownership and Lifecycle Rules
- the benchmark executable owns all configuration, payload, result, and client sockets
- TcpServer continues to own accepted TcpConnection bookkeeping and worker-loop teardown
- client sockets close before the benchmark asks the base loop to stop the server
- the base loop exits only after connected/disconnected callback counts converge and
  TcpServer reports no remaining base-loop connections
- benchmark callbacks capture only state that outlives the EventLoop and TcpServer

---

## 8. Evidence Boundaries
- use Release builds for recorded baseline numbers
- compare runs only when scenario parameters, build type, backend/completion mode,
  host context, and command are recorded
- working-set deltas are process-level observations and include allocator/runtime effects
- loopback results are regression baselines, not production network capacity claims
- IOCP single-versus-batched completion performance remains a future implementation comparison

---

## 9. Contract Guard
- `tests/cmake/test_core_benchmark_contract.py` verifies the opt-in CMake boundary,
  active intent, non-CTest status, scenario/schema fields, backend reporting,
  process-memory sampling, documentation commands, and CI guard parity
- `tests/ci/test_core_benchmark_workflow.py` verifies the manual-only trigger,
  paired Release platform jobs, fixed scenario set, JSON validation, and artifacts
- the guard runs in ordinary CI and long-soak preflight, but the benchmark executable
  is intentionally not run as a correctness gate

---

## 10. Review Checklist
- Does the change preserve the default-off and non-installed boundary?
- Is each reported number defined precisely enough to compare later?
- Are EventLoop, TcpServer, TcpConnection, and client-socket owners unambiguous?
- Can callbacks or driver threads race on benchmark-owned result state?
- Does slow-client output avoid claiming a memory cap that core does not implement?
- Are Release command, platform details, and raw JSON retained with recorded evidence?
