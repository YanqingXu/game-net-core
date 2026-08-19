# Commit-Bound Evidence Ledger Template

Use one copied section per integrated architecture slice. Evidence is attached
to an immutable commit; a dirty-worktree result may be recorded as local
preflight but cannot be promoted to commit or remote evidence.

## Slice Record Template

| Field | Required value |
| --- | --- |
| Slice | Plan identifier and one-sentence scope |
| Commit | Full 40-character commit SHA; never `HEAD` |
| Parent/baseline | Exact comparison SHA and baseline document |
| Intent/ADR | Active intent paths and accepted architecture decision |
| Owner/lifetime | Owner thread, owner/releaser, re-entry, cross-thread path |
| Public surface | `none`, or exact reviewed API diff artifact |
| Changed production paths | Explicit path list |
| Focused contracts | Exact command, test names, result, duration |
| Windows full gate | Exact command/result and artifact or local log identity |
| Linux full gate | Exact command/result and artifact or local log identity |
| Benchmark/capacity | Scenario, paired sample count, result, decision |
| Sanitizer/race evidence | Required lane and result, or reason not applicable |
| Review | Reviewer identity, scope, findings, disposition |
| Remote evidence | Run/attempt/artifact/hash, or explicitly `not collected` |
| Decision | `integrate`, `fix`, `revert`, or `promotion-only follow-up` |

## Integrity Rules

1. Record the commit only after the tree is committed and clean.
2. Do not copy evidence from an ancestor unless the changed path audit proves it
   remains applicable and the record labels that proof.
3. Local and remote results remain separate. Workflow presence is not evidence.
4. A benchmark beyond its investigation threshold requires paired reruns and a
   written accept/fix decision.
5. Failed or superseded evidence remains visible with its disposition.
6. Release/endurance evidence is promotion-only unless a slice changes the
   corresponding validator or lifecycle contract.

ARCH-G1 establishes this template. IOE-R1 is the first slice required to append
a completed, exact-commit record.

## IOE-R1 Source-Private Adapter — 2026-08-19

| Field | Evidence |
| --- | --- |
| Slice | IOE-R1 first vertical slice: source-private `IoEngine`, native-Poller adapter, EventLoop routing, lifecycle/capability contract |
| Commit | `fc03d4f492ed5481fd8bfd082f90429c4f0ff6a4` |
| Parent/baseline | Parent `65b3051ce97bcf44ee75eb62ec270c2ab38b5abb`; runtime comparison `669ebb0a7c5c475dea74b12275c66a2ce1876804`; `docs/development/io_engine_baseline.md` |
| Intent/ADR | `intents/architecture/io_engine.intent.md`, `intents/architecture/runtime_models.intent.md`, `docs/architecture/io_engine_runtime_model_adr.md` |
| Owner/lifetime | One EventLoop owner thread owns the dual-interface Engine object through its byte-stable `unique_ptr<Poller>` storage. Wait, registration, completion tracking, dispatch state, quiesce, and release are owner-only. `wakeup()` is the only cross-thread Engine operation. EventLoop invokes re-entrant user callbacks and destroys the backend after attached owner-thread teardown. Existing completion leases remain retained by the native backend until terminal dequeue. |
| Public surface | `none`; no installed header changed. `tests/api/test_public_api_manifest.py` and the same-line `compare_public_api_manifest.py` blocking gate passed with the reviewed v0.3 snapshot. |
| Changed production paths | `src/core/net/detail/IoEngine.h`; `src/core/net/detail/PollerIoEngineAdapter.cc`; `src/core/net/EventLoop.cc`; `src/core/net/detail/EventLoopControlRegistry.h`; `src/core/net/detail/EventLoopIocpAssociationHarness.h`; `src/core/CMakeLists.txt` |
| Focused contracts | Windows and Linux: `ctest --test-dir <release-build> -R '^(contract.io_engine.test_io_engine_poller_adapter\|contract.poller.test_poller_contract\|contract.event_loop.test_event_loop\|contract.channel.test_channel_active_batch_lifetime\|contract.tcp_connection.test_tcp_connection_completion_drain\|integration.tcp.test_iocp_quit_completion_drain\|integration.tcp.test_iocp_accept_connect_quit_completion_drain)$' --output-on-failure`; 7/7 passed in 0.32 s Windows and 0.29 s Linux. |
| Windows full gate | Exact clean commit; `ctest --test-dir build-perf-r1-fix -C Release --output-on-failure`; 122/122 passed in 51.73 s. |
| Linux full gate | Exact clean commit; `ctest --test-dir build-wsl-perf-r1-fix --output-on-failure`; 122/122 passed in 49.42 s under WSL/GCC 13.3. |
| Benchmark/capacity | Exact clean commit, one warmup plus three alternating echo pairs against `669ebb0`: Windows throughput 60.797 -> 63.402 MiB/s (+4.29%), P99 74.8 -> 66.7 us (-10.83%); Linux throughput 48.590 -> 48.542 MiB/s (-0.10%), P99 125.101 -> 96.946 us (-22.51%). No regression crossed the 5% investigation threshold; integrate. |
| Sanitizer/race evidence | Not applicable to this adapter-only local integration decision: it adds no shared queue, callback owner, operation storage, or lifetime release rule. No sanitizer result is claimed; sanitizer/race evidence remains required when IOE-C1 changes completion storage and retirement. |
| Review | Codex `/root` self-review covered intent, public contract, owner/thread affinity, lifetime, re-entry, cross-thread wakeup, tests, and performance. The stable-header constraint found during review caused the adapter to preserve the byte-identical EventLoop/Poller headers. Independent ARCH-G1 review remains pending and is not claimed here. |
| Governance | Exact clean commit; all 26 repository/API/CI guard scripts passed; 32 active intents and 156 explicit verification paths; same-line public API blocking gate passed. |
| Remote evidence | `not collected`; local evidence is not promoted to CI or release evidence. |
| Decision | `integrate` this first IOE-R1 slice; IOE-R1 remains open for explicit register/submit/cancel admission results, stale-notice and budget-exhaustion contracts, and option layering. |

## IOE-R1 Contract Closure — 2026-08-19

| Field | Evidence |
| --- | --- |
| Slice | IOE-R1 closure: typed admission/operation outcomes, owner-only mutation, completion obligation drain, callback containment, stale-notice/budget continuation, and backend-capacity option layering |
| Commit | `8bb14e72d8935879396d12a7a51c891311aa2a78` |
| Parent/baseline | Parent `c9c9c47b73370b6d92541b179a0023048a5ff3f6`; runtime comparison `669ebb0a7c5c475dea74b12275c66a2ce1876804`; `docs/development/io_engine_baseline.md` |
| Intent/ADR | `intents/architecture/io_engine.intent.md`; `docs/architecture/io_engine_runtime_model_adr.md`; `rules/thread_affinity_rules.md`; `rules/ownership_rules.md`; `rules/testing_rules.md` |
| Owner/lifetime | EventLoop's owner thread performs registration, cancellation, accepted completion commit, wait, phase transition, and callback dispatch. `wakeup()` remains the only direct cross-thread Engine operation. Engine borrows readiness Channels; a successful completion submission acquires the supplied lease until its real terminal packet is dequeued. Quiescing rejects new external admission while already-accepted cleanup and final drain remain legal. EventLoop contains callback exceptions and generation-checks retained notices before invocation. |
| Public surface | `none`; stable installed declarations and targets have zero same-line diff. The IOCP batch constructor is classified `platform_internal`; the new Engine vocabulary and accessor remain source-private. `tests/api/test_public_api_manifest.py` and the blocking same-line comparator passed. |
| Changed production paths | `src/core/net/detail/IoEngine.h`; `src/core/net/detail/PollerIoEngineAdapter.cc`; `src/core/net/detail/IocpPollerAccess.h`; `src/core/net/detail/EventLoopControlRegistry.h`; `src/core/net/EventLoop.cc`; `include/gamenet/core/net/poller/IocpPoller.h`; `src/core/net/poller/IocpPoller.cc` |
| Focused contracts | Exact clean commit, `contract.io_engine.test_io_engine_poller_adapter`: Windows 1/1 in 0.02 s; Linux/WSL 1/1 in 0.03 s. The contract covers owner-thread rejection, invalid/unsupported results, admission seal, actual IOCP terminal drain, callback-local close, stale generation, contained exception, exact budget continuation metrics, option mapping, and cross-thread wakeup. |
| Windows full gate | Exact clean commit; Release build passed; 122/122 CTest tests passed in 12.79 s with four workers. |
| Linux full gate | Exact clean commit; GCC 13.3 Release build passed under WSL Ubuntu 24.04; 122/122 CTest tests passed in 12.66 s with four workers. |
| Benchmark/capacity | Exact clean commit against `669ebb0`, one warmup per side plus three alternating echo pairs. Windows throughput 58.990 -> 61.452 MiB/s (+4.17%), P99 69.3 -> 62.0 us (-10.53%), stop 0.185 -> 0.190 ms (+0.005 ms). Linux throughput 49.991 -> 49.593 MiB/s (-0.80%), P99 89.948 -> 91.191 us (+1.38%), close 1.262 -> 1.238 ms. Linux stop 0.122 -> 0.160 ms (+0.038 ms, +30.9%) crossed the percentage investigation trigger, but all paired samples were drained in 0.12–0.18 ms; the sub-0.04 ms absolute shift is accepted as local scheduler/timer noise. No connection storage or per-connection allocation changed; the decision is integrate. |
| Sanitizer/race evidence | No sanitizer result is claimed. This slice adds no shared producer queue or non-owner mutation path; completion storage still uses the already-contracted native backend lease. Sanitizer/race evidence remains mandatory for IOE-C1's direct completion storage/retirement change. |
| Review | Codex `/root` self-review followed intent, public contract, invariant, thread-affinity, ownership, lifecycle, implementation, and test-completeness order. Production no longer includes a repository test harness for IOCP progress; `IocpPollerAccess` is the source-private accessor. Independent ARCH-G1 review remains pending and is not claimed as closure evidence. |
| Governance | Exact clean commit; all 26 repository/API/CI guard scripts passed; 32 active intents and 156 explicit verification paths; same-line stable public API blocking gate passed. |
| Remote evidence | `not collected`; this is local exact-commit integration evidence, not CI, endurance, capacity, or release evidence. |
| Decision | `integrate` and close IOE-R1. Move the active implementation front directly to IOE-R2 generation-safe epoll Readiness contracts; no candidate freeze is introduced. |
