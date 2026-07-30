---
status: active
target: gamenet_core_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
---

# Use-Case Intent: Core Performance Baseline

## 1. Intent
The core performance baseline is a reproducible, opt-in engineering tool for
measuring the current Reactor/TCP implementation on Linux epoll and Windows
IOCP. The executable records raw measurements without embedding timing
thresholds or expanding the installed `GameNet::core` API; the Phase 6 workflow
applies reviewed same-runner relative regression budgets outside the executable.

---

## 2. Responsibilities
- build only when `GAMENET_BUILD_BENCHMARKS=ON`
- exercise the public TcpServer/TcpConnection path over loopback TCP
- measure echo round-trip latency, completed-message rate, and bidirectional
  application-byte throughput
- measure process working-set growth while holding idle connections
- measure process CPU during an explicit idle observation interval without a
  recurring benchmark coordination poll
- separately report connection establishment, client-close/base-map
  convergence, and graceful server/worker stop duration
- optionally create connections through a bounded simultaneous client-worker
  set so accept-burst elapsed time is measured without changing sequential
  canonical defaults
- generate paced, bounded-batch connection churn with exact attempt,
  accept/failure, close, rate, and worker-distribution accounting
- observe working-set growth, bounded output admission, high/low-water read
  throttling, and recovery for slow-reading clients
- report connection count, EventLoop worker count, backend, completion mode,
  build type, IOCP accept depth, parameters, and measurements as one versioned
  JSON document
- provide a manual-only workflow that runs a 1/2/4-worker, 256/1,024-connection,
  and 4/16-slow-client Release matrix three times for both baseline and candidate
- return a non-zero exit code when setup, I/O, timeout, or schema production fails

---

## 3. Non-Responsibilities
- is not a CTest and embeds no pass/fail performance threshold
- does not claim cross-machine scores are directly comparable
- does not install a benchmark library or add public headers
- does not implement a backpressure policy, memory cap, metrics subsystem, or
  alternate IOCP completion-drain strategy; it observes the Core policy
  configured on `TcpServer`
- does not benchmark HTTP, protocol framing, TLS, UDP, KCP, or game-server layers

---

## 4. Scenario Contracts

### `echo`
- establish the requested loopback connections before the timed interval
- run sequential request/echo round trips per connection from blocking client workers
- record every completed RTT sample and report nearest-rank P50/P99/P999 in
  microseconds
- define one completed request/echo RTT as one message; report messages per
  second as exact completed RTTs divided by the common timed interval
- define throughput as request bytes plus echoed response bytes divided by the
  same timed interval
- require exact configured RTT and application-byte counts, internally
  consistent rate/throughput, and ordered P50 <= P99 <= P999 before accepting
  an artifact

### `connections`
- sample process working set after server startup and before client creation
- default to one client connect worker; an explicit bounded
  `--connect-concurrency` value starts that many workers together and must not
  exceed the requested connection count
- report and apply a bounded `--iocp-accept-depth` through the public
  TcpServer configuration point; it defaults to four
- an explicit connections-only preload mode creates every client before the
  base EventLoop starts, then measures from loop start through all connection
  callbacks so queued accept-drain work is isolated from SYN creation jitter
- compare the preloaded ready-backlog profile with the live concurrent-connect
  profile before attributing connection churn to the base loop; live
  client/kernel handshake time is not an accept-loop bottleneck measurement
- hold the requested accepted connections through a configurable settle interval
- report precise connection-establishment seconds/rate before the settle
  interval, process CPU seconds/percent during that idle interval,
  client-close/base-map convergence, graceful server/worker stop outcome/time,
  and before/after/delta working set plus approximate delta per connection
- retain legacy `elapsed_seconds` semantics, which include the settle interval,
  so existing same-runner regression budgets do not silently change meaning
- close connection-scenario client sockets abortively after measurement so
  repeated local burst samples do not accumulate client-side `TIME_WAIT`

### `connection-churn`
- derive an exact attempt count from configured target connections/second and
  duration; `--connections` is the maximum live batch, not the total attempt
  count
- pace each bounded batch against one monotonic start time, use at most
  `--connect-concurrency` persistent client connector workers, and wait for
  cumulative accept callbacks before closing that batch
- use nonblocking connect completion with an explicit per-attempt monotonic
  deadline; timeout or socket failure increments client-connect-failed instead
  of leaving a generator worker blocked without a bound
- close churn clients abortively, wait for cumulative disconnect callbacks,
  and do not reuse the next batch's capacity until the prior batch converges
- report attempted, accepted, client-connect-failed, and closed counts with the
  common elapsed interval and internally consistent attempt/accept/close rates
- report per-batch connect, ready-accept callback, close, and schedule-lag
  nearest-rank P99/maximum so client handshake delay is not misidentified as
  base-loop ready-backlog delay
- record accepted-connection counts per distinct owner EventLoop, including
  every configured worker, and report `(max - min) / mean` worker skew
- account every churn close callback by the fixed
  `TcpConnectionCloseReason` key set so terminal causes sum exactly to closed
- require attempted = accepted + client-connect-failed and accepted = closed;
  a strict capacity capture may additionally require zero connect failures
- begin final close-convergence timing immediately before the last batch closes,
  then prove the same typed graceful server/worker stop contract as every other
  successful scenario

### `slow-client`
- clients connect with a deliberately small receive buffer and do not read
- each accepted connection calls `trySend()` with the configured payload from
  its owner loop and records the exact admission result
- hold reads until the memory sample, then drain accepted output so write
  completion can observe read-throttle recovery
- report requested, accepted, and rejected bytes; rejection reasons; configured
  low/high/hard output thresholds and input limit; pending-output peak;
  pause/resume observations; high-water callback count; and working-set change
- the `--high-water` scenario parameter configures both read-throttle policy and
  high-water notification; the low-water threshold is the recorded half-value
- fail semantic validation if requested bytes do not equal accepted plus
  rejected bytes

---

## 5. Output Contract
- stdout contains exactly one JSON document with schema
  `gamenet.core_benchmark.v2`; diagnostics use stderr
- every scenario reports `platform`, `backend`, `completion_mode`, and `build_type`
- every scenario reports configured `connections`, `connect_concurrency`,
  `iocp_accept_depth`, `preload_before_loop`, and `event_loop_threads`
- measurement keys remain present across scenarios; values that do not apply are `null`
- the regression runner may ingest the frozen `v1` baseline schema and current
  `v2` candidate schema, but candidate artifacts and semantic validation require
  the v2 bounded-admission fields
- Windows reports `get_queued_completion_status_ex_batch_64`; Linux reports
  the batched `epoll_wait` mode. Historical Windows Core Preview evidence keeps
  its distinct `single_get_queued_completion_status` value
- raw JSON output is the durable comparison artifact; documentation summaries
  must identify platform, build type, command, and date

---

## 6. Threading Rules
- the process main thread owns and destroys EventLoop and TcpServer
- TcpServer callbacks execute only on their assigned connection owner loops
- each blocking benchmark client socket is owned by exactly one driver thread;
  parallel connection creation uses one distinct pre-sized socket slot per
  worker index
- churn client workers mutate only distinct pre-sized batch slots; callback
  owner-loop distribution is aggregated under benchmark-owned synchronization
- benchmark coordination uses atomics, a condition variable, and isolated result storage
- every condition-variable predicate transition is published while holding the
  same benchmark mutex used by its waiter; atomics remain snapshot-friendly
  but do not substitute for the no-lost-wakeup protocol
- driver/disconnect completion coalesces through one capacity-aware base-loop
  executor task; the idle observation window has no recurring coordination
  timer that would manufacture idle CPU
- no benchmark worker directly mutates loop-owned server or connection state
- final connection-map inspection and graceful-stop initiation execute on the
  base EventLoop thread; a benchmark-owned waiter observes only the shared
  stop future and posts the terminal sample back to that owner loop

---

## 7. Ownership and Lifecycle Rules
- the benchmark executable owns all configuration, payload, result, and client sockets
- TcpServer continues to own accepted TcpConnection bookkeeping and worker-loop teardown
- client sockets close before the benchmark asks the base loop to stop the server
- the base loop exits only after connected/disconnected callback counts
  converge, TcpServer reports no remaining base-loop connections, and the
  graceful stop future proves worker-loop join completion
- the stop waiter owns only its thread, a copied future/executor capability,
  and value timestamps; it owns no TcpServer, EventLoop, connection, or socket
- benchmark callbacks capture only state that outlives the EventLoop and TcpServer

---

## 8. Evidence Boundaries
- use Release builds for recorded baseline numbers
- compare runs only when scenario parameters, build type, backend/completion mode,
  host context, and command are recorded
- production-candidate regression compares baseline and candidate only on the
  same runner, uses three-sample medians, and retains both raw sample sets
- small-echo capacity evidence spans fixed 64/256/1,024-byte payloads and
  1/2/4 worker loops; payload and worker count remain part of the comparison key
- working-set deltas are process-level observations and include allocator/runtime effects
- idle CPU is process CPU divided by monotonic wall time for the configured
  observation interval; it includes the server's real idle runtime overhead
  but excludes an artificial recurring benchmark poll
- loopback results are regression baselines, not production network capacity claims
- IOCP single-versus-batched evidence must retain distinct completion-mode
  values and be compared only on the same runner with matching scenario inputs
- the frozen v1 baseline and v2 candidate are compared only on their common
  reviewed performance metrics; v2 admission/accounting fields are validated
  independently and are not inferred for v1 samples

---

## 9. Contract Guard
- `tests/cmake/test_core_benchmark_contract.py` verifies the opt-in CMake boundary,
  active intent, non-CTest status, scenario/schema fields, backend reporting,
  process-memory sampling, documentation commands, and CI guard parity
- `tests/ci/test_core_benchmark_workflow.py` verifies the manual-only trigger,
  paired Release platform jobs, expanded fixed matrix, JSON validation, and artifacts
- the guard runs in ordinary CI and long-soak preflight, but the benchmark executable
  is intentionally not run as a correctness gate

---

## 10. Review Checklist
- Does the change preserve the default-off and non-installed boundary?
- Is each reported number defined precisely enough to compare later?
- Are EventLoop, TcpServer, TcpConnection, and client-socket owners unambiguous?
- Can callbacks or driver threads race on benchmark-owned result state?
- Does slow-client output describe the configured Core hard limit without
  turning one loopback run into a production capacity claim?
- Does slow-client accounting prove requested = accepted + rejected and expose
  the configured Core hard limit?
- Are Release command, platform details, and raw JSON retained with recorded evidence?
