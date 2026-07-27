---
status: active
target: gamenet_phase4_benchmark
migration_source: mini_trantor
promote_gate: none
artifact_kind: benchmark
migration_mode: redesign
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: tests/integration/benchmark/test_game_server_metrics_smoke.cpp;tests/integration/benchmark/test_fps_like_broadcast_latency.cpp
---

# Use-Case Intent: Phase 4 Performance Baseline

## Intent

The Phase 4 performance baseline is a reproducible, opt-in engineering tool for
measuring protocol framing, logic-queue delay, and broadcast fanout. The
executable records raw evidence without embedded thresholds; the Phase 6
workflow applies reviewed same-runner relative regression budgets without
expanding any installed library API.

## Scenario Contracts

### `framing`

- Decode a known number of valid length-prefixed frames through the public
  `PacketFramer` API with explicit frame and byte budgets.
- Verify the exact decoded frame and payload-byte totals before reporting.
- Report wire-byte throughput and decoded frames per second. Encoding and input
  construction occur before the timed interval.

### `logic-queue`

- The process main thread owns one `EventLoop` and `LogicLoop`; configured
  producer threads use only the cross-thread-safe `submit` operation.
- Record enqueue-to-handler lag for every accepted command and report P50/P99.
- Report queue depth and byte high-water marks from the synchronized queue
  snapshot, plus accepted/rejected totals.
- The run succeeds only after every configured command is accepted and handled;
  a timeout or unexpected rejection is an error, not a partial score.

### `broadcast-fanout`

- The process main thread is the management-loop owner and constructs online
  sessions whose endpoints are distributed over explicit worker EventLoops.
- Each fanout iteration waits for every scheduled endpoint send before starting
  the next iteration, making route-to-send and full-fanout completion latency
  unambiguous.
- Report route, dispatch-queue, endpoint-delivery, and fanout-completion
  P50/P99; accepted/dropped totals; scheduled-task total/high water; and process
  working-set before/after/peak values.
- A dedicated sampler thread observes process working set only; it never reads
  or mutates EventLoop, session, endpoint, router, or dispatcher state.

## Threading And Ownership

- The benchmark executable owns all configuration, payloads, samples, worker
  threads, EventLoops, endpoints, and sessions.
- Framing runs entirely on the main thread and has no callback or re-entry path.
- Logic handlers run on the logic owner loop and may update main-thread-owned
  samples because that owner is the process main thread. Producer threads are
  joined before `LogicLoop` destruction.
- Broadcast endpoint `send` callbacks execute only on their endpoint executor.
  Cross-loop completion and sample aggregation use atomics, a mutex, and a
  condition variable; no callback waits synchronously for another loop.
- Broadcast work drains completely, sessions/endpoints are released, and worker
  EventLoops are stopped before benchmark-owned coordination state is destroyed.

## Output And Evidence Contract

- stdout contains exactly one `gamenet.phase4_benchmark.v1` JSON document;
  diagnostics use stderr and failures return non-zero.
- Every document identifies scenario, platform, backend, build type, parameters,
  elapsed time, and stable nullable measurement keys.
- The executable builds only with `GAMENET_BUILD_BENCHMARKS=ON`, is not a CTest,
  and is neither installed nor exported.
- Recorded evidence uses Release builds. Results are comparable only when the
  commit, platform/backend, compiler, parameters, and command match.
- Working-set high water is process-level sampled RSS, not allocator accounting
  and not a claim that broadcast has a hard memory cap.
- The manual benchmark workflow runs framing plus two Logic and two Broadcast
  scales three times for both the fixed baseline and candidate on each Linux
  epoll and Windows IOCP runner.
- A shared cross-platform validator rejects missing/non-finite required
  measurements (allowing valid zero latency/RSS delta) and inconsistent
  operation, accepted, rejected, batching, or working-set counts. It writes a
  semantic manifest containing commit SHA, run id, job, artifact name,
  parameters, and raw-file SHA-256 hashes.
- Each platform artifact also carries the shared CI job manifest, which binds
  the actual checkout, candidate/PR/merge identities, run attempt, runner,
  commands, toolchain, and every evidence-file hash. Artifact names include the
  job, tested SHA, run id, and attempt.
- A final workflow job downloads both platform artifacts and independently
  verifies their common identity, successful job status, file hashes, exact
  Linux/epoll plus Windows/IOCP pair, fixed scenario set, and identical
  parameters. It emits one paired evidence manifest. These checks prove
  provenance, semantic consistency, matrix sample hashes, and separate
  same-runner performance-budget success; it never compares Linux scores with
  Windows scores.

## Verification

- `tests/cmake/test_phase4_benchmark_contract.py` verifies the opt-in target,
  scenario/schema fields, ownership markers, memory sampler, and documentation.
- `tests/ci/test_phase4_benchmark_workflow.py` verifies the paired Release jobs,
  fixed scenario set, schema validation, and same-commit artifact names.
- `tools/validate_phase4_benchmark.py` validates both platforms with one
  scenario-aware implementation and produces the immutable evidence manifest.
- `tools/verify_phase4_benchmark_evidence_set.py` verifies the two job manifests
  and semantic manifests form one same-run, same-attempt, cross-platform pair.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: owner-loop dispatch, bounded batches, queue-lag observation,
  high-fanout latency sampling, and raw evidence separated from correctness.
- Deferred from the source design: business simulation, AOI selection, exporter
  integration, adaptive thresholds, and production capacity claims remain out
  of scope.
- Dropped behavior: none; the migrated harness makes its process-memory and
  lifecycle boundaries explicit and adds a standalone framing throughput case.
