# Roadmap

game-net-core is the component-split migration target for `mini_trantor`.
The roadmap keeps that migration staged so the networking core becomes stable
before protocol, transport, game-foundation, or experimental modules are added.
See `migration_status.md` for the current checked state of these phases.

## Current Roadmap Checkpoint — 2026-08-11

- The reviewed local implementation checkpoint is
  `main@9d2a5be0eb5439399f27c2f53ec1bf985c7de1d0`; at audit time the local
  `origin/main` reference was `7fa6922725304fe0d1c80a806b19a0173cdf9c3e`.
- M3-R1/P1-01 is closed at independently reviewed checkpoint `95a6ab5`; M3-R2
  and its EventLoopThreadPool negative contracts are committed at `12adb00`.
- The current inventory is 120 CTest tests: 8 unit, 99 contract, and 13
  integration, with 93 threading and 98 lifecycle labels. The `9d2a5be` tree
  passed a fresh local Windows/IOCP Release 120/120 gate, all 36 repository/
  API/CI guards, and stable/provisional install consumers 2/2; its API-R1
  reviewed-surface diff is strictly empty.
- `9d2a5be` closes the post-review TcpServer owner-establishment bookkeeping
  leak and TcpClient construction-failure request wedge with deterministic
  recovery contracts. It has no same-SHA remote or Linux evidence and is not a
  frozen production candidate.
- No final `v0.3.0-production-candidate` is frozen. Earlier benchmark,
  capacity, endurance, and candidate runs remain historical infrastructure or
  superseded-candidate evidence and do not qualify the eventual candidate.
- API-R1 is complete: the independent reviewer closed all initial blockers and
  returned `APPROVE`; historical and strict same-line zero-diff artifacts are
  archived. REL-C1 candidate freeze is the next task, followed by same-SHA
  evidence.

## Phase 1: Project Skeleton

- Initialize CMake, README, repository rules, and documentation structure.
- Establish public header and implementation layout.

## Phase 2: Reactor / TCP Core

- Migrate Logger, Timestamp, noncopyable, socket primitives, Channel, Poller,
  Wakeup, TimerQueue, EventLoop, and TCP lifecycle components.
- Keep the first target focused on `gamenet_core`.

## Phase 3: Targets and Tests

- Split unit, contract, and integration tests.
- Add minimal echo-server coverage for the TCP path.
- Export and install `GameNet::core` so downstream components can consume the
  split package through `find_package(GameNetCore)`.

## Phase 3.5: Core Preview Hardening

Historical closure recorded for `v0.1.0-core-preview`:

- make `TcpConnection::connected()` / `disconnected()` atomic snapshot observers;
- make TcpConnection callback, context, and socket-option mutation owner-loop-only;
- marshal TcpServer callback installation/replacement to each connection owner loop;
- fix the Linux repeated-connect failure found in `ci` run `29059799283` by
  releasing Connector member ownership before deferred Channel destruction;
- fix the second Linux repeated-connect ordering failure found in `ci` run
  `29073362905` by admitting one generation-tagged `connect()` request per
  pending/active lifecycle and releasing it after terminal failure/teardown;
- define Logger runtime replacement, callback snapshot/concurrency, re-entry,
  and capture-lifetime semantics with a threading contract;
- add a default-off, non-CTest core benchmark with versioned JSON output for
  echo throughput/latency, connection working-set growth, worker-loop scaling,
  and slow-client accumulation;
- record the first local Windows MSVC Release baseline, including the current
  single-completion IOCP mode and raw JSON evidence;
- classify all 52 formal intents with machine-checked active/deferred/legacy
  metadata so historical mini_trantor stages and test counts cannot authorize
  current work;
- replace self-referential HEAD documentation with immutable validation records;
- pass local Windows Debug, Release, install-consumer, and 5-repeat threading preflight;
- validate candidate `a7fd77cbd2140041cebb3f900d5c609fafc2adad` in `ci`
  run `29076601085` with all five Linux/Windows jobs green;
- pass remote `long-soak` run `29077148022` with all 46 threading tests at
  repeat 50 and a 60-second per-test timeout;
- retain same-SHA Linux epoll and Windows IOCP Release JSON artifacts from
  `core-benchmark` run `29077151229`;
- merge PR #2 without rewriting the validated candidate, pass all five jobs in
  main `ci` run `29079836593`, and publish annotated tag
  `v0.1.0-core-preview` at release commit
  `c4818d4b3956c85830e04d4a1f32df4ad701d453`.

This was an annotated preview tag only; no GitHub Release page or release
assets were published for `v0.1.0-core-preview`.

Phase 3.5 has no remaining gates. The release evidence chain is:

1. code candidate `a7fd77cbd2140041cebb3f900d5c609fafc2adad`;
2. PR five-job CI run `29076601085`, long-soak run `29077148022`, and
   benchmark run `29077151229`;
3. release merge commit `c4818d4b3956c85830e04d4a1f32df4ad701d453`,
   main five-job CI run `29079836593`, and tag `v0.1.0-core-preview`.

The tag-only release record and limitations are preserved in
`docs/development/releases/v0.1.0-core-preview.md`.

## Phase 4: Higher-level Modules

The Phase 4 foundation and audit remediation were published in
`v0.2.0-phase4-preview`, with
each module behind its own intent, invariant, contract, and test gate:

- [x] Add bounded length-delimited PacketFramer parsing, continuation budgets,
  wrap-around coverage, deterministic round-trip smoke, and a real libFuzzer
  target with binary corpus and dictionary.
- [x] Add TransportEndpoint and a TCP adapter using lifetime-safe owner-loop
  executors without changing core lifecycle ownership.
- [x] Add network-only PlayerSession/SessionManager state with management-loop-
  only session access, transport identity uniqueness, collision, rebind, and
  shutdown tests. Deterministic lifecycle coverage overlaps two live
  heartbeat/offline producers with owner-loop drain and proves final cleanup
  only after both producer sentinels execute.
- [x] Add a bounded GameCommandQueue and one-shot fixed-tick LogicLoop with
  deterministic admission, re-entry, stop, and accounting contracts.
- [x] Add a non-installed pipeline demo whose IO, management, and logic stages
  run on three physical loops with authentication and shutdown/handoff tests.
  Atomic callback revocation is checked before each side effect, synchronous
  Logic-stage stop is safe when management and logic share a loop, and an exact
  AUTH-plus-command batch locks deterministic same-batch admission.
- [x] Add Router-only broadcast plans, owner-loop grouping, task budgets, exact
  backpressure metrics, large-fanout coverage, and real multi-loop TCP tests
  with repeated disconnect/reconnect windows.
- [ ] Keep UDP/KCP work experimental and independently gated.

Before candidate freeze, the local `final-v4` preflight passed 85/85 configured
tests in Windows Debug (36.47 seconds), Windows Release (36.97 seconds), and Linux Clang Release
(34.76 seconds), all 27 Python guards, and exact-version external Release
package consumers on Linux Clang and Windows MSVC for all six exported targets.
Full Windows MSVC Debug AddressSanitizer passes 85/85 in 43.19 seconds and Linux
Clang 19 ASan/UBSan passes 85/85 in 36.18 seconds. Linux Clang 19 TSan passes all
61 threading tests in 35.63 seconds after repairing a TimerQueue test-fixture
race. Local Clang libFuzzer completes exactly 1000 runs with the binary
corpus/dictionary and no `max_total_time`. The 61-test threading slice passes
repeat 50 (3,050 executions, zero failures, 1,777.76 seconds), and the eight-test
Pipeline/Broadcast slice passes repeat 50 (400 executions, zero failures, 54.16
seconds). Both structured `gamenet.ctest_repeat_evidence.v1` manifests report
success against inventory SHA-256
`37ee7fb3572c911fa771ba42ce1fcb91a252bc2c78c56b98b280f5305c77a09a`.
The Linux and Windows Release install consumers each passed 1/1. Both platforms
also produced all three fixed Release Phase 4 benchmark scenarios with
`status: ok` and passed the shared validator. These local results supported the
functional candidate subsequently committed and pushed as
`5ebad2c1a4a9487437340935e21f7468140c7e8d`; they are not substituted for the
remote evidence below.

Intent governance now resolves all 25 active targets and 74 explicit
verification paths. Seven enriched Phase 4 intents require artifact kind,
provenance, and non-empty verification, while 16 frozen Core library intents
retain only their documented metadata exemption and must still resolve to the
real installed Core target. The production one-way dependency rule is derived
from the actual configured CMake target graph, including transitive reachability
and negative direct/transitive reverse-dependency fixtures.

Main CI is defined as six producer jobs plus one aggregation-only evidence
gate, not seven platform jobs. Producer manifests are bound to the candidate,
checkout, run, and attempt; the aggregate `gamenet.ci_evidence_set.v1` also
recomputes file hashes and proves exact inventory/JUnit selections. Long-soak
writes `gamenet.ctest_repeat_evidence.v1` summaries for every selected test and
exact repeat count. The Phase 4 benchmark workflow uses two platform producers
and a third aggregation-only gate whose
`gamenet.phase4_benchmark_pair_evidence.v1` requires one Linux/epoll and one
Windows/IOCP result from the same run with identical scenario parameters.

Functional candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d` is committed and
pushed, and was the Draft PR #4 head when candidate evidence was produced.
Pull-request `ci` run
`29160903594` validated GitHub merge-ref
`e461b597f2642e000717f536f3b430b804ba26ad` while binding candidate and PR-head
identity to `5ebad2c1a4a9487437340935e21f7468140c7e8d`; all six producers and the
aggregate evidence gate passed 7/7. Manual `long-soak` run `29161167423`
completed the exact 3,050/3,050 threading and 400/400 Pipeline/Broadcast
executions and uploaded the verified evidence bundle. Manual benchmark run
`29161168417` completed both platform producers and the paired evidence gate
successfully.

Draft PR #4 head `0d62054e148a1c95793799eb88856363ac6843d3` and five-job `ci`
run `29147391402` (#32) are retained only as pre-hardening history. They do not
validate candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d` or replace its
current evidence chain.

PR #4 advanced through final evidence-only head `4abd5960...`, whose run
`29162961320` passed all six producers plus aggregate. It was then moved to Ready
and owner-authorized for a merge commit. Merge/release commit `7668d6b8...` has
the same tree as that PR head, and main push run `29168786199` passed 7/7 against
the exact release commit. Annotated tag `v0.2.0-phase4-preview` and the
[formal GitHub prerelease](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview)
are published with canonical archives and `SHA256SUMS`. This closes the Phase 4
Preview publication plan, not production readiness or API/ABI stability. PR #4
had zero submitted GitHub reviews, which remains a process limitation.
Experimental HTTP, RPC, UDP/KCP, TLS, and coroutine work remains deferred behind
separate intent promotion.

## Phase 5: Production Hardening

Implementation hardening after the Phase 4 Preview:

- [x] Suppress Linux per-write `SIGPIPE` without changing process-global signal
  disposition; peer-close write failure remains an explicit connection error.
- [x] Bound per-connection input buffers and admitted output bytes across
  owner-loop buffering and accepted cross-thread sends, expose overload
  results, and pause/resume reads with owner-loop high/low-water hysteresis.
- [x] Bound EventLoop cross-thread task admission and drain work per iteration,
  with explicit rejection and a bounded legacy/control reserve.
- [x] Add completion-aware graceful server drain with timeout and forced-close
  fallback.
- [x] Replace recoverable runtime socket/accept failures with explicit results
  and policy hooks.
- [x] Define callback exception containment and connection/server failure policy.
- [x] Add connection admission limits, unauthenticated timeout, and basic abuse
  controls.
- [x] Validate the earlier frozen production-hardening candidate `be749ad`
  through Linux/Windows sanitizer, long-soak, benchmark, package, and evidence
  gates. This is historical evidence superseded by later runtime changes.
- [x] Close M3-R1 accepted-fd/TcpConnection construction ownership at
  independently reviewed checkpoint `95a6ab5`.
- [x] Close M3-R2 EventLoopThreadPool configuration/state rejection locally at
  committed implementation checkpoint `12adb00`.
- [ ] Freeze the post-M3-R2 candidate and regenerate clean, same-SHA platform,
  sanitizer, package, performance, capacity, and endurance evidence.

## Phase 6: Production Candidate

In progress after production hardening:

- [x] Define the production-candidate intent, compatibility policy, supported
  API classes, and release exit gates.
- [x] Add a versioned public API manifest and an automated compatibility guard.
- [x] Implement provisional MetricsExporter and non-I/O producer adapters with
  deterministic snapshots; defer hot-path API promotion to metrics-on evidence.
- [x] Add same-platform performance regression budgets and retained trend
  evidence for the fixed Core and Phase 4 Release scenarios.
- [x] Add structured fault-injection coverage and validate the real 24-hour
  candidate plus 72-hour paired Linux endurance infrastructure at `b344318`.
- [x] Synchronize roadmap, assessment, plan, README, and migration status at
  GOV-R2 without promoting historical evidence to the current checkpoint.
- [x] Complete independent stable Core API review, archive the historical and
  same-line diffs, and enforce zero stable-surface drift (API-R1).
- [ ] Requalify performance validators/budgets and release/endurance tooling on
  the single frozen candidate SHA.
- [ ] Freeze one v0.3.0 production candidate, complete Linux/Windows evidence,
  publish the release, and merge the validated candidate to `main`.
