---
status: active
target: gamenet_hot_path_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: redesign
source_commit: 202bf9f993575d144c4c440a3972dd80733da0a7
source_paths: benchmarks/core/main.cpp;benchmarks/phase4/main.cpp;benchmarks/capacity/main.cpp;benchmarks/runtime_profiles/multi_io_queued.cpp
---

# Use-Case Intent: HP0 Hot-Path Cost Baseline

## Intent

HP0 creates a reproducible cost ledger before any HP1-HP8 data-path promotion
or default-path change. A bounded experimental-prestudy exception may explore
isolated alternatives while HP0 evidence is pending, but cannot replace the
baseline or advance a milestone.
The opt-in suite reuses the current callback and mutex-backed implementations,
keeps each child benchmark's semantic validator as the correctness authority,
and records comparable structural and latency costs in
`gamenet.hot_path_cost.v1`. It changes no installed target or runtime behavior.

## Scenario Contract

The preregistered inventory covers:

- `framing.callback-copy`: current `PacketFramer::push()` framing;
- `network-logic.callback-mutex`: real NetworkLoop to LogicShard command and
  owner-return flow using the current `GameCommandQueue` path;
- `readiness-send.callback-borrowed`: real TCP ping-pong through epoll on
  Linux and IOCP on Windows, covering readiness/completion dispatch and send;
- `broadcast.current-owner-tasks`: real owner-grouped broadcast fanout;
- `connection-capacity.current`: 10k real TCP connection establishment,
  idle-memory observation, close convergence, and server shutdown;
- `overload-recovery.current`: real slow-reader broadcast pressure and stable
  recovery with exact terminal and zero-residue checks.

Every scenario preregisters one primary metric, guardrail metrics, exact
arguments, and the HP slice for which it is a baseline. Inventory drift is an
intent change and cannot occur inside a performance comparison.

## Output Contract

- The suite manifest is exactly `gamenet.hot_path_cost.v1` and preserves every
  raw child JSON file plus SHA-256.
- The fixed cost-key set is: cycles, instructions, LLC load misses,
  allocations, copied bytes, syscalls, wakeups, generic posts, cross-domain
  handoffs, P50/P99/P999, maximum queue age, RSS, overload recovery, and
  shutdown convergence.
- A counter that the current child or platform cannot observe is `null` and
  has a non-empty unavailable reason. Zero means an observed exact zero.
- One unrecorded warmup precedes at least ten recorded samples per scenario.
  Sampling is round-robin by scenario so temperature and temporal drift are
  visible; raw parameters must remain identical across samples.
- The manifest records the clean exact commit, working-tree state, platform,
  backend, native/WSL classification, runner id, CPU model, affinity, frequency
  policy, compiler, build type, command, executable paths and hashes.
- The runner independently reads `git rev-parse HEAD` and porcelain worktree
  status from `--source-root`; caller-provided identity cannot override either.
- `development` evidence may be dirty, WSL, or omit hardware counters, but is
  never promotion eligible. `fixed-lab` evidence fails closed on any such
  limitation or on a missing required cost.

## Threading And Ownership

- The Python suite process owns configuration, child processes, raw files, and
  the final manifest; it mutates no EventLoop or connection state.
- Each child executable retains the owner/threading rules from its active
  benchmark intent. The suite waits synchronously for child termination and
  never injects a callback into a child EventLoop.
- There is no cross-thread shared mutable state in the suite. Child stdout is
  decoded as strict UTF-8; diagnostic stderr is retained with byte escapes.

## Lifecycle And Failure

- A nonzero child exit, invalid JSON, semantic mismatch, parameter drift,
  missing raw file, hash mismatch, or incomplete scenario aborts the suite.
- The output directory must be new or empty, so stale samples cannot satisfy a
  later run.
- An interrupted run may leave raw development artifacts but cannot produce a
  successful manifest.
- Fixed-lab evidence requires all child lifecycle checks to pass, including
  accepted-work accounting, overload recovery, close convergence, graceful
  stop, and zero retained fixed storage where the child schema exposes it.

## Fixed Laboratory Boundary

- Linux evidence runs natively on the epoll runner; Windows evidence runs
  natively on the IOCP runner. WSL is development-only.
- The manual workflow targets named self-hosted performance runners. Hosted CI
  executes static guards only and cannot mint promotion evidence.
- CPU affinity and frequency policy are explicit non-empty reviewed strings;
  the runner configuration is an evidence input, not an inferred default.
- Recorded baselines use Release binaries from the same exact clean commit.
  HP1 promotion and integration cannot start until both platform ledgers
  validate and are indexed in the exact-commit evidence ledger.

## Experimental Prestudy Exception

- HP0 remains the unique performance-promotion and integration front while its
  fixed-lab evidence is incomplete.
- At most one HP1-HP8 implementation prototype may carry the
  `EXPERIMENTAL-PRESTUDY` label at a time, in parallel with HP0 laboratory
  evidence. The label is not a milestone status.
- A prototype is default-off, non-installed, non-exported, absent from public
  manifests and release assets, and not linked into the default production
  Core data path. It cannot change the production epoll/IOCP choice or current
  callback-and-mutex baseline.
- Design, intent, rules, contracts, and benchmark scaffolding may be prepared
  out of milestone order. Runtime prototypes must declare and satisfy their
  real technical prerequisites; mocks cannot prove a downstream composed
  optimization.
- Before implementation, each runtime prototype names its owner, ownership and
  release path, callback re-entry behavior, cross-thread marshal, bounded
  admission, shutdown settlement, and exact regression test file.
- All prestudy measurements are development evidence. Before HP0 closes they
  may conclude only `KEEP-EXPERIMENTAL`, `REJECT`, or `DEFER`; they cannot
  produce `INTEGRATE`, switch a default path, add an installed API, or satisfy
  a 5%/3% promotion gate.
- After HP0 closes, any surviving candidate re-enters its formal HP slice,
  reruns against the fixed baseline, and earns verification from scratch.

## Verification

- `tests/cmake/test_hot_path_benchmark_contract.py` verifies the active intent,
  default-off/non-installed target, inventory coverage, frozen schema keys,
  runner and validator fail-closed rules, and documentation.
- `tests/ci/test_hot_path_benchmark_workflow.py` verifies the manual-only named
  self-hosted Linux/Windows jobs, Release commands, ten samples, validation,
  and immutable artifact retention.
- `tools/validate_hot_path_cost.py` independently validates the manifest,
  inventory, raw files, hashes, platform identity, sample counts, parameter
  stability, cost availability, and child semantic invariants.

## Non-Goals

- HP0 does not implement `PacketView`, mailbox/outbox, slot-arena epoll,
  segmented sends, credit leases, pools, coroutines, or io_uring promotion.
- HP0 does not add a public runtime factory, backend selector, mailbox, or
  installed benchmark component.
- A single echo peak or hosted-CI number is not production capacity evidence.
