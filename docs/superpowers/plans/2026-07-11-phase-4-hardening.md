# Phase 4 Hardening Implementation Plan

**Goal:** Close the audit blockers in draft PR #4 and produce a mergeable, verifiable Phase 4 preview without expanding deferred protocol/transport scope.

**Architecture:** Preserve `GameNet::core → protocol/transport → game_session/game_logic/broadcast → example` as a one-way dependency graph. Each module follows Intent → invariants → threading → ownership → contracts/tests → implementation. PR #4 remained an umbrella feature PR rather than dependency-ordered stacked review units. The final artifacts follow the intent method, but the two large commits cannot retrospectively prove intent/test-first chronology; this remains a review-process limitation rather than a code-contract blocker.

**Baseline:** Core tag `v0.1.0-core-preview` (`c4818d4`); validated Phase 4 functional candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d`; release commit `7668d6b82a0d815ccd79f83c572bc0a36bcceea0`; tag `v0.2.0-phase4-preview`; audit report `docs/development/phase4_audit_2026-07-11.md`.

**Non-goals:** HTTP, WebSocket, RPC, TLS, coroutine, UDP, KCP, AOI/room/business state, production-readiness claims, and an installed all-in-one Pipeline library.

**Status convention (2026-07-12):** `[x]` means the implementation, evidence,
or publication action exists and has been checked against its recorded identity.
`[ ]` identifies work intentionally left deferred beyond this Preview. The older
`0d62054` CI run remains historical entry evidence only.

Evidence is deliberately layered. Local preflight establishes that the
worktree is a viable candidate; producer manifests bind files to a checkout,
candidate, GitHub/merge SHA, run, attempt, job, and canonical artifact name;
aggregate manifests prove that the required producer set shares one identity.
None of those contracts creates a candidate commit, starts a remote run, or
authorizes a tag or Release.

---

## Mandatory change gate

Every merge unit must state in its PR description:

1. Which EventLoop/thread owns each mutable object?
2. Who owns and releases it, and what invalidates observers?
3. Which callbacks can re-enter, and which locks are held at that point?
4. Which operations are cross-thread and how are they marshaled?
5. Which exact test files prove normal, failure, race, and teardown paths?

No task is complete merely because an existing CTest remains green. New contracts must fail against the pre-fix behavior and pass after implementation.

## Merge unit 0: Intent semantics and traceability guard

**Intent/rules:** `intents/README.md`, the six Phase 4 module/example intents,
the Phase 4 performance-baseline intent, `rules/review_rules.md`, and
`rules/testing_rules.md`.

**Files:**

- Modify: `intents/README.md`
- Modify: `intents/modules/{packet_framer,transport_endpoint,player_session,logic_loop,broadcast}.intent.md`
- Modify: `intents/usecases/game_server_pipeline_demo.intent.md`
- Modify: `intents/modules/game_backpressure_policy.intent.md`
- Modify: `tests/scope/test_intent_metadata.py`
- Create: `tests/scope/test_intent_semantics.py`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/workflows/long-soak.yml`

- [x] Extend active metadata so an intent distinguishes installed library from example and names a real CMake target.
- [x] Change Pipeline metadata from nonexistent `GameNet::game` to its real example artifact without creating a fake installed game target.
- [x] Record `mini_trantor` source commit/path and migration mode for redesigned Phase 4 modules.
- [x] Record kept, deferred, and dropped invariants, including fuzz, lifetime token, parser budget, backpressure, and stress evidence.
- [x] Mark the active LogicLoop/Broadcast portions derived from deferred game backpressure while leaving end-to-end policy deferred.
- [x] Add failing guard fixtures for nonexistent target, wrong artifact kind, missing test path, unregistered CTest, and missing migration provenance.
- [x] Implement semantic target/test/provenance checks and register them in Linux, Windows, and long-soak guards.
- [x] Derive semantic validation from the active catalogs/front matter: validate
  25 active targets and 74 explicit verification paths while keeping the 16
  frozen Core library intents as explicit metadata exemptions.
- [x] Validate the configured production CMake link graph, including negative
  direct and transitive dependency-direction fixtures.
- [x] Verify all 59 formal intents appear exactly once in active/deferred/legacy catalogs.

**Gate:** No active intent may name a nonexistent artifact or unregistered test. Deferred/legacy files must not authorize implementation.

## Merge unit 1: PacketFramer admission and real fuzz

**Owner:** One connection I/O loop owns and mutates a framer. Returned frames are values; no callback or re-entry path exists.

**Files:**

- Modify: `intents/modules/packet_framer.intent.md`
- Modify: `include/gamenet/protocol/PacketFramer.h`
- Modify: `src/protocol/PacketFramer.cc`
- Modify: `tests/contract/protocol/test_packet_framer_contract.cpp`
- Rename: `tests/contract/protocol/test_packet_framer_fuzz_smoke.cpp` to deterministic round-trip terminology
- Create: `tests/contract/protocol/test_packet_framer_budget.cpp`
- Create: `tests/fuzz/protocol/fuzz_packet_framer.cpp`
- Create: `tests/fuzz/corpus/packet_framer/*`
- Modify: `src/protocol/CMakeLists.txt`, `tests/CMakeLists.txt`, top-level `CMakeLists.txt`

- [x] Define `maxBufferedBytes`, `maxFramesPerPush`, and frame-byte processing budget in the active intent and API.
- [x] Define all-or-reject input admission: `push()` accepts the complete bounded input or faults before append, and reports retained work through `needsContinuation` without an unbounded caller-input copy.
- [x] Add failing tests for zero-length floods, many sticky frames, budget boundary ±1, partial maximum frame, continuation order, overflow fault, and reset.
- [x] Implement incremental bounded parsing, ring-buffered unread storage, and explicit result/failure statuses.
- [x] Keep deterministic valid-frame/chunk property smoke as a normal contract with accurate naming.
- [x] Add an optional libFuzzer target with `LLVMFuzzerTestOneInput`, malformed input, bounded continuation, reset checks, and one-shot/chunked differential decoding.
- [x] Execute the real libFuzzer target under ASan/UBSan for the final candidate SHA and preserve the command/result. Main CI run `29160903594` records 1,000 units in the Linux ASan/UBSan producer manifest and aggregate evidence set.
- [x] Add framing throughput snapshot support without a performance threshold.
- Deferred non-gate: an allocation-specific framing profile may be added later;
  current Phase 4 performance intent requires throughput, while process RSS is
  explicitly a coarse Broadcast observation rather than allocator accounting.

**Gate:** Protocol contracts and real fuzz smoke pass; one call cannot monopolize the I/O loop or grow internal storage beyond its configured bound.

## Merge unit 2: TransportEndpoint lifetime and scheduling

**Owner:** The upper layer owns endpoint objects. TcpConnection/Core retains socket/connection ownership. Endpoint observation must not extend or outlive an unsafe EventLoop reference.

**Files:**

- Modify: `intents/modules/transport_endpoint.intent.md`
- Modify: `include/gamenet/transport/{TransportEndpoint,TcpTransportEndpoint}.h`
- Modify: `src/transport/TcpTransportEndpoint.cc`
- Modify: `tests/contract/transport/test_tcp_transport_endpoint_contract.cpp`
- Create: `tests/contract/transport/test_tcp_transport_endpoint_lifetime.cpp`

- [x] Choose and document an opaque executor contract that rejects admission without exposing a cached raw EventLoop pointer.
- [x] Expose stable owner identity for Broadcast grouping without exposing an independently dereferenceable loop lifetime assumption.
- [x] Specify `isOpen()` as a cross-thread atomic snapshot observer.
- [x] Add contracts for wrong-thread close, endpoint outliving connection/loop, and `isOpen()` racing close.
- [x] Add an explicit accepted-before-stop queued send/close drain contract and
  preserve owner-only endpoint operations during EventLoop's final accepted-work drain.
- [x] Implement safe scheduling, send, close, and closed/unavailable results without changing TcpConnection ownership semantics.
- [x] Prove endpoint operations add no callback/re-entry surface and scheduled callers own every captured value.

**Gate:** Endpoint/connection/loop teardown produces no raw-loop dereference; Debug/Release/TSan transport contracts pass.

## Merge unit 3: SessionManager index and queued-work lifecycle

**Owner:** One management EventLoop owns all session mutable state and both indexes. I/O loops only post value/shared-endpoint work and never wait.

**Files:**

- Modify: `intents/modules/player_session.intent.md`
- Modify: `include/gamenet/game_session/SessionManager.h`
- Modify: `src/game_session/SessionManager.cc`
- Modify: `tests/contract/game_session/test_session_manager_contract.cpp`
- Create: `tests/contract/game_session/test_session_manager_lifecycle.cpp`

- [x] Define transport-id/endpoint collision policy before implementation; reject the newcomer and preserve the existing binding.
- [x] Add tests for two players sharing one transport id, same-player/different-endpoint collision, rebind collision, ProtocolError close, and both index views after rejection.
- [x] Add tests for queued authenticate/offline/heartbeat followed by manager shutdown/destruction before execution.
- [x] Protect queued work with executor admission plus a weak lifetime token whose strong guard spans authenticate and callback execution.
- [x] Make shutdown revoke new admission and define cancellation/no-op callback behavior.
- [x] Preserve atomic index consistency before authentication callbacks run and expose manager-owned sessions as read-only public views.
- [x] Add callback re-entry coverage: lookup, heartbeat, rebind/offline, and endpoint close scheduling while no manager lock is held.

**Gate:** A player and transport each index at most one live session; queued work cannot access destroyed manager state; TSan and repeat-soak pass.

## Merge unit 4: LogicLoop lifecycle and genuine concurrency

**Owner:** One logic EventLoop owns lifecycle, drain, handler, output, metric, and timer state. `submit()` and snapshots are synchronized cross-thread operations.

**Files:**

- Modify: `intents/modules/logic_loop.intent.md`
- Modify: `include/gamenet/game_logic/{GameCommandQueue,LogicLoop}.h`
- Modify: `src/game_logic/{GameCommandQueue,LogicLoop}.cc`
- Modify: `tests/contract/game_logic/test_logic_loop_contract.cpp`
- Create: `tests/contract/game_logic/test_logic_loop_concurrency.cpp`
- Create: `tests/contract/game_logic/test_logic_loop_lifecycle.cpp`

- [x] Define explicit `Created → Running → Stopping → Stopped` one-shot semantics; restart after stop is rejected.
- [x] Define stop backlog policy as close admission + cancel timer + discard queued commands with dropped command/byte metrics while completing the already-committed drain batch.
- [x] Add state-transition tests for duplicate start, idempotent stop, restart rejection, committed-batch completion, and stop with backlog.
- [x] Add a direct start-without-handler failure assertion.
- [x] Replace join-before-loop coverage with latches that prove multi-producer submit overlaps owner-loop drain and verify unique command ids.
- [x] Add handler/output/metric re-entry tests that call `submit()` without a queue lock held.
- [x] Protect timer dereference with owner-thread destruction plus revocable
  lifetime admission, make public lifecycle observation atomic, and test
  destruction while a tick timer is pending.
- [x] Add queue lag, drain/drop, and queue high-water benchmark observations without performance thresholds.
- Deferred non-gate: per-tick execution duration can be added in later profiling;
  the active closure intent requires queue lag, drain/drop, and high-water data.

**Gate:** The test demonstrates real overlap, every accepted command is handled at most once, stop reports all discarded backlog, and TSan/repeat-50 remain clean.

## Merge unit 5: Pipeline authentication state and coordinated shutdown

**Owner:** Per-connection framing/authentication state belongs to the I/O loop; SessionManager state to management loop; LogicLoop state to logic loop. Cross-loop work is value-based and asynchronous.

**Files:**

- Modify: `intents/usecases/game_server_pipeline_demo.intent.md`
- Modify: `examples/game_server_pipeline_demo/{GameServerPipeline.h,GameServerPipeline.cc}`
- Modify: `tests/integration/game_pipeline/test_game_server_pipeline.cpp`
- Create: `tests/integration/game_pipeline/test_game_server_pipeline_auth_state.cpp`
- Create: `tests/integration/game_pipeline/test_game_server_pipeline_shutdown.cpp`
- Modify: `examples/CMakeLists.txt`, `tests/CMakeLists.txt`

- [x] Define `Unauthenticated`, `Authenticating`, `Online`, and `Closing` states plus allowed events/transitions.
- [x] Define bounded pending frame/byte behavior during Authenticating and exact overflow close reason.
- [x] Add a real-TCP `AUTH + command` same-write test and require ordered `AUTH_OK`, then response.
- [x] Add duplicate AUTH, pending overflow, and disconnect-before-auth-complete tests.
- [x] Add a deterministic SessionManager duplicate-login rejection path and
  prove queued pre-auth commands are discarded without a response.
- [x] Implement connection-name/transport-endpoint validation so stale authentication cannot revive or send on a replacement connection.
- [x] Guard Pipeline callbacks with independent executor/state captures and a revocable lifetime token spanning management-side dereference.
- [x] Treat a strong callback-state reference as storage only, not execution
  permission: every I/O, management, timer, logic-output, and endpoint side
  effect rechecks the atomic revocation state.
- [x] Add a three-physical-loop fixture and assert I/O → management → logic → endpoint-owner stages.
- [x] Make `stop()` cancellation-convergent: revoke queued auth/output admission,
  synchronously stop/destroy LogicLoop, clear management state, and initiate
  server close. Hold an active worker callback across member teardown to prove
  it uses independent state; a public close-completion future remains outside
  this example's scope.
- [x] Cover synchronous `stop()` re-entry from the Logic stage when management
  and logic share one loop, and block an endpoint observer across revocation to
  prove no post-stop send is admitted.

**Gate:** Same-batch AUTH/command succeeds by contract; disconnect and shutdown races do not access stale state under ASan/UBSan, TSan, and repeat-soak.

## Merge unit 6: Broadcast plan, delivery, and backpressure evidence

**Owner:** Management loop owns routing decisions. Dispatcher tasks execute on each endpoint owner context. Payload/endpoint values are owned by the plan/task; loop ownership is never transferred.

**Files:**

- Modify: `intents/modules/broadcast.intent.md`
- Modify: `include/gamenet/broadcast/{BroadcastTypes,BroadcastRouter,BroadcastDispatcher}.h`
- Modify: `src/broadcast/{BroadcastRouter,BroadcastDispatcher}.cc`
- Modify: `tests/contract/broadcast/test_broadcast_contract.cpp`
- Modify: `tests/integration/broadcast/test_broadcast_tcp_multi_loop.cpp`

- [x] Make `BroadcastPlan` move-only and router-created so public callers cannot bypass route-time fanout/byte/dedup invariants.
- [x] Remove dispatcher raw EventLoop dereference and schedule through the TransportEndpoint executor contract.
- [x] Add independent exact-count tests for duplicate, offline, fanout-hard, byte-hard, low-priority-soft, dispatch-byte, endpoint-closed, owner-unavailable, invalid-plan, and send-rejected reasons.
- [x] Preserve shared payload, per-owner grouping, per-task endpoint/byte limits, and per-endpoint ordering.
- [x] Add real `TcpTransportEndpoint` delivery across two distinct server I/O loops.
- [x] Add close-between-route-and-dispatch and owner-expiry contract coverage.
- [x] Add eight real-TCP disconnect/reconnect cycles across two server worker
  loops and prove stale endpoints do not survive into the next routed plan.
- [x] Emit versioned route/dispatch/fanout latency, task/RSS high-water, accepted, and dropped benchmark snapshots.

**Gate:** Full reason matrix is exact; one dead/slow endpoint does not invalidate other batches; real multi-loop TSan and soak pass.

## Merge unit 7: Phase 4 evidence-only closure

**Scope:** No new feature behavior. Only packaging, CI, evidence, version, documentation, and release metadata.

**Files:**

- Modify: `.github/workflows/{ci,long-soak}.yml`
- Create or modify: Phase 4 fuzz/benchmark workflow and workflow contract tests
- Modify: `tests/cmake/install_consumer/*`
- Modify: `CMakeLists.txt`
- Modify: `README.md`, `docs/{roadmap,migration_status}.md`, `docs/development/ci.md`

- [x] Execute install consumer binaries on Linux and Windows; compilation alone is insufficient.
- [x] Add a Windows MSVC Release IOCP CTest/install-consumer job alongside Debug and execute the Release consumer locally.
- [x] Run real PacketFramer fuzz smoke under sanitizers and keep deterministic smoke naming distinct.
- [x] Add `gamenet.ctest_repeat_evidence.v1` verification that derives the exact
  selected tests from the 85-test inventory, requires every selected test to
  appear exactly 50 times with no failure/unknown result, and hashes the raw log
  and inventory.
- [x] Refresh the complete 61-test threading inventory and focused 8-test
  Pipeline/Broadcast slice at repeat 50 after the final Pipeline
  callback/lifetime fixes. The pre-freeze worktree passed 3,050 executions
  in 1,777.76s and 400 executions in 54.16s respectively, and both
  `gamenet.ctest_repeat_evidence.v1` summaries validated successfully.
- [x] Re-run the complete 61-test threading inventory and focused 8-test
  Pipeline/Broadcast slice at repeat 50 on the immutable final candidate, then
  preserve the remote manifest, raw logs, structured repeat summaries, and run
  metadata. Run `29161167423` records 3,050/3,050 and 400/400 exact executions.
- [x] Produce local Linux/Windows Phase 4 benchmark artifacts for framing, logic
  queue lag/high-water, and broadcast fanout latency/memory; paired same-SHA
  remote artifacts remain separately gated.
- [x] Add the two-layer benchmark evidence contract: per-platform
  `gamenet.ci_evidence.v1` wrappers plus a
  `gamenet.phase4_benchmark_pair_evidence.v1` gate that verifies the exact Linux
  epoll/Windows IOCP job pair, common run/SHA identity, raw hashes, scenario
  order, runner/platform identity, and identical parameters.
- [x] Produce and preserve the paired Linux/Windows benchmark artifacts from the
  same immutable final candidate SHA. Run `29161168417` produced Linux/epoll,
  Windows/IOCP, and a successful `gamenet.phase4_benchmark_pair_evidence.v1`.
- [x] Add six canonical main-CI producer artifacts and an aggregation-only
  `ci-evidence-set` job. `gamenet.ci_evidence_set.v1` accepts exactly
  `linux-cmake`, `linux-asan-ubsan`, `linux-tsan`, `linux-release`,
  `windows-msvc`, and `windows-msvc-release` from one run/attempt/SHA identity,
  rehashes every declared file, and checks exact CTest/JUnit inventories.
- [x] Commit and push an immutable Phase 4 candidate, then record candidate SHA,
  PR head SHA, merge-ref SHA, run/job IDs, test labels/counts, artifact names,
  dates, durations, and conclusions. The functional candidate is `5ebad2c1...`;
  the authoritative details are in `docs/development/phase4_evidence_ledger.md`.
- [x] Complete current-worktree single-run Debug/Release, ASan/UBSan, TSan,
  repository-guard, install-consumer, and dependency-direction preflight. This
  remains mutable local evidence and does not satisfy the final-candidate gate.
- [x] Run the corresponding clean final-candidate jobs remotely and preserve the
  six-producer aggregate manifest. PR run `29160903594` tested merge-ref
  `e461b597...`, bound candidate/PR head `5ebad2c1...`, and passed all six
  producers plus the aggregate gate.
- [x] Set the locally frozen Phase 4 project/package version to `0.2.0`; tag and
  publication identity remain gated by the immutable final candidate.
- [x] Merge the validated candidate without rewriting its final PR-head tree,
  pass main-branch run `29168786199`, then create annotated tag and formal
  GitHub Release `v0.2.0-phase4-preview`.
- [x] Include known limitations, unstable API list, validation links,
  benchmark/soak artifacts, downstream upgrade notes, and canonical archive
  checksums in the Release.

**Gate:** Functional evidence belongs to the recorded candidate or its explicitly
bound PR merge-ref; branch-local or Phase 3.5 evidence cannot substitute for
Phase 4 validation. An evidence-documentation descendant must be proven
non-functional and pass the necessary PR/main gates before release; identity is
never inferred merely from ancestry.

## Current evidence boundary

The local `final-v4` evidence below was the preflight used to freeze the
functional candidate. It remains useful diagnostic history:

- Windows MSVC Debug passing 85/85 in 36.47s and Release passing 85/85 in
  36.97s;
- Linux Clang 19 Release passing 85/85 in 34.76s, plus exact-version Release
  consumers passing 1/1 on Linux and Windows; the current-worktree Windows
  Release external consumer completed 1/1 in 0.02s;
- a full MSVC Debug ASan run passing 85/85 in 43.19s after it exposed and
  drove repair of a Connector ConnectEx late-completion Channel UAF;
- a full Linux Clang 19 ASan/UBSan run passing 85/85 in 36.18s, and Linux Clang
  19 TSan passing the complete 61-test threading inventory in 35.63s after
  exposing and driving repair of a TimerQueue test-fixture race;
- the real PacketFramer libFuzzer completing 1000 ASan/UBSan runs with six
  binary seeds and its dictionary;
- the post-fix 61-test threading selection passing repeat 50 for 3,050
  executions in 1,777.76s, with its
  `gamenet.ctest_repeat_evidence.v1` summary validating successfully;
- the post-fix eight-test Pipeline/Broadcast selection passing repeat 50 for
  400 executions in 54.16s, with its
  `gamenet.ctest_repeat_evidence.v1` summary validating successfully;
- Linux and Windows local snapshots each producing all three fixed Phase 4
  Release `gamenet.phase4_benchmark.v1` scenarios with `status: ok` and passing
  the shared semantic/count-invariant validator.

The immutable functional candidate is now
`5ebad2c1a4a9487437340935e21f7468140c7e8d`. Its authoritative remote evidence
is layered as follows:

- PR main CI run `29160903594`: candidate/PR head `5ebad2c1...`, tested merge-ref
  `e461b597...`, six producers plus aggregate 7/7 success;
- long-soak run `29161167423`: direct candidate checkout, 61×50 and 8×50 exact
  repeat manifests success;
- benchmark run `29161168417`: direct candidate checkout, Linux/epoll and
  Windows/IOCP pair manifest success.

The evidence contracts themselves are locally implemented and guarded:

- main CI has six producer jobs plus one aggregation-only evidence gate; the
  aggregate is not a seventh platform/test producer;
- long-soak preserves the full inventory, two raw repeat logs, two
  `gamenet.ctest_repeat_evidence.v1` summaries, toolchain data, and a canonical
  job manifest;
- core-benchmark has two platform producers plus a paired-evidence gate whose
  output is `gamenet.phase4_benchmark_pair_evidence.v1`.

The candidate-level CI, TSan, sanitizer-backed libFuzzer, repeat-50, and paired
benchmark gates are closed. Publication closure is also complete: PR #4 was
moved to Ready and merged under explicit owner authorization, main CI
`29168786199` validated exact commit `7668d6b8...`, annotated tag object
`b76077f...` peels to that commit, and the formal GitHub Preview Release carries
the required evidence links, limitations, unstable APIs, downstream notes, and
verified source/archive checksums. GitHub recorded zero submitted reviews; this
is retained as a process limitation and is not represented as completed review.

## Standard verification commands

```powershell
cmake -S . -B build-phase4-hardening -DGAMENET_BUILD_TESTING=ON
cmake --build build-phase4-hardening --config Debug --parallel
ctest --test-dir build-phase4-hardening -C Debug --output-on-failure
cmake --build build-phase4-hardening --config Release --parallel
ctest --test-dir build-phase4-hardening -C Release --output-on-failure
python tests/scope/test_intent_metadata.py
python tests/scope/test_intent_consistency.py
python tests/scope/test_intent_semantics.py
python tests/scope/test_scope_guard.py
python tools/check_scope_boundaries.py
git diff --check
```

Sanitizer, fuzz, repeat-50, install-consumer, and benchmark commands must be copied verbatim into the immutable evidence ledger with their commit SHA and result.
