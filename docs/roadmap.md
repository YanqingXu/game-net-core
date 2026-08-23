# Roadmap

game-net-core is the component-split migration target for `mini_trantor`.
The roadmap keeps that migration staged so the networking core becomes stable
before protocol, transport, game-foundation, or experimental modules are added.
See `migration_status.md` for the current checked state of these phases.

## Current Roadmap Checkpoint — 2026-08-24

- Historical REL-C1 implementation checkpoint
  `669ebb0a7c5c475dea74b12275c66a2ce1876804` is recorded by the commit peeled
  from annotated tag `v0.3.0-rel-c1-refreeze-5`; the tag object and remote ref
  remain immutable evidence. It supersedes
  `v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`.
  It is no longer a development freeze point.
- M3-R1/P1-01 is closed at independently reviewed checkpoint `95a6ab5`; M3-R2
  and its EventLoopThreadPool negative contracts are committed at `12adb00`.
- The current inventory is 130 CTest tests: 8 unit, 108 contract, and 14
  integration, with 103 threading and 108 lifecycle labels. The IOE-R2
  Readiness Engine and IOE-C1 operation-model contracts pass on Windows/IOCP
  and Linux/epoll; the committed operation-model checkpoint is `f4074400` and
  the PERF-R1 reviewed-surface diff remains strictly empty.
- `9d2a5be` closes the post-review TcpServer owner-establishment bookkeeping
  leak and TcpClient construction-failure request wedge with deterministic
  recovery contracts and remains the immutable reviewed-surface checkpoint.
- The superseded `refreeze-1` candidate passed REL-V1 and REL-V2 before PERF-R1
  exposed comparator/high-fd/profile defects. `refreeze-2` passed local REL-V1,
  but remote runs exposed tag checkout, revision-wide sample-order bias,
  capacity snapshot interference, stdout flush, and diagnostic decoding gaps.
  `3d54c08` closes those evidence-tool defects without changing the reviewed
  stable API. Its local Windows paired regression/Core-capacity matrices and
  cross-platform candidate-10k repeats pass, but are not release evidence.
  `refreeze-3` then passed local REL-V1, while REL-V2 run `32039657783` proved
  that `actions/checkout@v4` flattened the local annotated tag after fetching
  it. The refreeze-4 workflows restore the exact remote tag object before all
  repository guards without changing any remote tag. Refreeze-4 then completed
  REL-V1, REL-V2, and its paired Core benchmark, but capacity run
  `32043877128` failed twice on Windows when a client I/O deadline made exact
  accept accounting unreachable. Implementation checkpoint `669ebb0` adds the
  missing connect/accept/echo/close phase barrier without relaxing that
  deadline; 9 Windows and 3 Linux local candidate samples pass.
- API-R1 is complete: the independent reviewer closed all initial blockers and
  returned `APPROVE`; PERF-R1's additive `setSendBufferSize` surface is recorded
  as source-compatible at `api-r1-perf-r1-reviewed-surface`.
- Candidate freeze is retired as a development gate. IOE-R1 is closed at
  `8bb14e72d8935879396d12a7a51c891311aa2a78`; IOE-R2 is closed at
  `6f45aa6e78152b8fd86df925962e580101b2f2ee`; IOE-C1's operation model is
  committed at `f407440084c48f324ab3a65912443bb257aa8e77`, with direct
  read/write closed at `82d89831de81522ecd25c8eedd42f395e2a07613` and all-kind
  direct consumers at `d0f2e07c3e3798605ac5d60ee33a7f7689fd542c`.
  IOE-C1 is closed at `c2d7e9d65dab8b110b58c20594b0cebd5199cf26`: legacy
  completion translation/storage is retired, shutdown phases are monotonic,
  and dual-platform/API/sanitizer/performance plus exact-commit evidence passes.
  RTM-R1 Profile A (`SingleLoopInlineEvent`) is integrated at
  `adb8b483d9b00ed0e9723321f2d7438e43a5e478` as a runnable, non-installed
  zero-worker composition. Profile B (`MultiIoQueuedEvent`) is integrated at
  `633d61315a9e28db643ee91214dc2f26a9b64630` with bounded coalesced handoff,
  generation-safe owner return, typed recovery, and directional numbers.
  Profile C (`MultiIoDedicatedFixedTick`) is integrated at
  `da57edc887421503e93ca743afb8a3373642c878` with authoritative fixed-rate
  cadence, bounded catch-up/skip, bounded tick drain, generation-safe owner
  return, and dual-platform directional numbers. RTM-R2 Profile D is closed
  at `b3b184b1cd8d28a256cffc60679ab832373dcfef` with independent placement/sharding, bounded
  cell-local Hybrid ordering, isolated saturation, and no installed API.
  IOE-X1 is closed at `d3b31c5c4e7966553094f7e42cf74f1b49a11077`: the
  Linux-only non-installed Engine has real one-shot Accept/Recv/Send,
  SQ-full/cancel/lease/final-drain contracts, ASan/UBSan coverage, and a
  structured opt-in directional benchmark while epoll remains production
  default/fallback. The cross-Profile real-TCP integration contract now drives
  the same echo/stop lifecycle through A/B/C/D and closes the common-capability
  review as `NO-PROMOTION`; it also closes a Profile C cadence-stop summary
  publication race. IOE-X2–X9 close the EventLoop Pump, real-TCP driver, shared
  Hub, fixed capacity/churn, semantic adapter, graceful half-close, bounded
  cross-thread admission, and source-private listener/Accept ownership.
  IOE-X10 is closed at `f5d39b800b4dd943531670aa09840c931c3dee4d`:
  all ten fixed-protocol formal samples pass correctness, capacity, recovery,
  and zero-residue validation, producing a narrow `PROMOTE` for later
  source-private shaping. Independent ARCH-G1 is `APPROVE` after owner-
  observation, destruction, and construction-rollback blockers were fixed.
  No public selector, installed io_uring target, or production replacement is
  authorized. M1 and M2 are closed. Promotion commit `0c30124` passed the
  complete same-commit CI/capacity/benchmark/repeat/1h/3h chain and produced
  the internally packaged, SPDX-indexed
  `v0.3.0-internal-candidate.1`. It remains all-rights-reserved and is not an
  external release. M3 real gateway integration is closed at private gateway
  commit `0a8fe1e`; the exact gateway `4e2457e` / Core `736a090` process passed
  its uninterrupted 1-hour gate. M4 is closed: final promotion commit
  `8e4a6ed` passed the full fresh non-waived matrix, deterministic Apache-2.0
  packaging, final extracted/upgrade consumers, annotated tag, stable Release,
  and fresh-download verification. M5 is closed with a second
  `NO-PROMOTION`, an official Profile load-selection guide, zero public API
  drift, and no empty v0.4 release. IOE-X11 is closed at `013fecf`, IOE-X12 at
  `5be30e7`, IOE-X13 at `5484d7a`, IOE-X14 at `351b3c0`, and IOE-X15/M6 at
  `43795e8`. The explicit Linux-only component is prepared but no preview tag
  or Release was published. M7 external-first Lua/typed RPC validation is
  closed. M7-G0 at `44493b1` records external implementation `DEFER` and
  shared RPC `NO-PROMOTION`; gateway M4 closure `d03cacd` and M7 governance
  `92a2607` set only the external adapter to historical `RESUME`. Gateway
  implementation `e43393c` and closure `588acd0`, plus exact independent
  `b525416` 8/8 revalidation, close M7 as `NO-PROMOTION`; shared promotion stays
  closed, and no RPC/Lua surface or empty v0.6 release was added. M8 then
  compared current Core callback/executor/timer semantics, callback-only
  gateway `588acd0`, and Actor-bound `YanGameServer@b525416`; Core 3/3 and Yan
  9/9 evidence proved valid but non-substitutable contracts, so M8 closed as
  `NO-PROMOTION` with all six async intents deferred and no empty v0.7 release.
  M9 TLS/WebSocket/DNS evidence review is the active front. The cancelled
  `a89e2b0` endurance checkpoint remains historical only.

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
- [x] Retire candidate freeze as a development gate; retain old tags and runs as
  immutable history while moving validation to continuous exact-commit gates.

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
- [x] Add explicit owner-approved candidate and release endurance waivers for
  environments that omit long-duration evidence while preserving visible
  missing-evidence metadata.
- [x] Synchronize roadmap, assessment, plan, README, and migration status at
  GOV-R2 without promoting historical evidence to the current checkpoint.
- [x] Complete independent stable Core API review, archive the historical and
  same-line diffs, and enforce zero stable-surface drift (API-R1).
- [ ] Keep performance validators, capacity and release/endurance tooling green
  as a continuous evidence lane rather than a single frozen-candidate project.
- [x] When external promotion is desired, select a current-main promotion
  commit, complete Linux/Windows and endurance evidence, decide licensing, and
  publish without stopping subsequent main development.

## Phase 7: I/O Engine and Runtime Profiles

M1 and M2 are closed. `v0.3.0-internal-candidate.1@0c30124` passed its complete
same-commit 1h/3h and package/evidence gates. The 2026-08-22 cancelled
checkpoint remains historical `NO-PROMOTION` evidence. M3 real gateway
integration is closed. M4 is closed at stable Apache-2.0
`v0.3.0@8e4a6ed`: the complete promotion matrix, deterministic assets, final
Linux/Windows extracted and upgrade consumers, annotated tag, stable Release,
and all-asset redownload verification passed. M5 repeated the common-capability
review against the real gateway and closed as a second `NO-PROMOTION`; the
Profile load-selection guide is published and all four recipes remain
non-installed. IOE-X11 is closed at
`013fecfe81277845eb3e60ccf5fe0205b753858d`, and IOE-X12 is closed at
`5be30e701c61f8d6700bcc4be6bc0ef152120fb8`. IOE-X13 is closed at
`5484d7a89b01597824bc860e4d2d3cf3cfd45a82`, and IOE-X14 is closed at
`351b3c0e476a462265016d53361a02b5f2c51611`. IOE-X15 and M6 are closed at
`43795e841ba2a279ed6a3d5d831d60a9f2a25570`; M7 external-first Lua/typed RPC
closed as `NO-PROMOTION` at gateway `588acd0`, M8 async/coroutine promotion
closed as `NO-PROMOTION` against Core `e5ea9ef` and YanGame `b525416`, M9
evidence review is the current front, and no Core evidence task is running.

- [x] ARCH-G1: independent review is `APPROVE` at the IOE-X10 checkpoint after
  all non-waivable ownership/thread-affinity/lifecycle blockers were closed.
- [x] IOE-R1: the source-private Engine seam, typed admission/operation results,
  owner/lifetime contracts, Poller adapter, option layering, and dual-platform
  evidence are closed at `8bb14e72d8935879396d12a7a51c891311aa2a78` without
  stable public-surface drift.
- [x] IOE-R2: move epoll into an explicit Readiness Engine with generation-safe
  registrations; closed at `6f45aa6e78152b8fd86df925962e580101b2f2ee`.
- [x] IOE-C1: native typed notices, all four direct consumers, removal of the
  legacy fake translation/runtime Channel storage, and monotonic shutdown are
  integrated at `c2d7e9d65dab8b110b58c20594b0cebd5199cf26` with
  dual-platform/API/sanitizer/performance and exact-commit evidence.
- [x] RTM-R1: validate three TCP-only provisional Profiles in parallel with the
  Engine line.
  - [x] Profile A `SingleLoopInlineEvent`: integrated at
    `adb8b483d9b00ed0e9723321f2d7438e43a5e478` with bounded inline dispatch,
    terminal overload/overrun handling, zero cross-domain handoff, runnable
    echo, dual-platform full gates, and no installed API.
  - [x] Profile B `MultiIoQueuedEvent`: integrated at
    `633d61315a9e28db643ee91214dc2f26a9b64630` with two network owners, one
    logic owner, bounded queue/drain, exact producer coalescing metrics,
    dual-platform full gates, and a non-installed benchmark/example.
  - [x] Profile C `MultiIoDedicatedFixedTick`: integrated at
    `da57edc887421503e93ca743afb8a3373642c878` with authoritative fixed-rate
    cadence, bounded catch-up/skip, bounded tick drain, dual-platform full
    gates, and a non-installed benchmark/example.
- [x] RTM-R2: bounded logic sharding and Hybrid execution are integrated as the
  non-installed Profile D vertical slice after Profiles A/B/C established
  contract and performance evidence.
- [x] M5: repeat the cross-Profile review against private gateway `0a8fe1e` and
  installed Core `736a090`; close as a second `NO-PROMOTION`, publish the
  workload-based Profile guide, retain zero installed Runtime API, and create
  no empty v0.4 release.
- [x] M6/IOE-X11: compose the listener, Hub, and semantic adapter into one
  source-private single-owner server without changing production `TcpServer`;
  closed at `013fecfe81277845eb3e60ccf5fe0205b753858d`.
- [x] M6/IOE-X12: transfer each accepted fd exactly once from the accept owner
  through bounded admission to one worker-owned Hub, with typed rollback on
  post/admission/shutdown failure and no established-connection owner migration;
  closed at `5be30e701c61f8d6700bcc4be6bc0ef152120fb8`.
- [x] M6/IOE-X13: add one-shot Connect, timeout/retry/cancel/stale-attempt
  settlement, callback re-entry and owner-quit convergence, then compare the
  source-private client adapter with production `TcpClient` observations;
  closed at `5484d7a89b01597824bc860e4d2d3cf3cfd45a82`.
- [x] M6/IOE-X14: drive one semantic server/client contract across epoll, IOCP,
  and io_uring while preserving backend-specific readiness/completion mechanics
  and comparing backpressure, close, half-close, admission, and final drain;
  closed at `351b3c0e476a462265016d53361a02b5f2c51611`.
- [x] M6/IOE-X15: install an explicit Linux-only experimental io_uring target,
  façades, manifest, package consumer, and version notes while keeping opt-in
  disabled by default and the stable `TcpServer` free of a backend selector;
  closed at `43795e841ba2a279ed6a3d5d831d60a9f2a25570` without publishing a tag or
  GitHub Release.
- [x] M7: validate owner-isolated Lua execution cells and callback/value typed
  RPC in the gateway plus a second independent consumer before promoting any
  shared protocol surface; closed as `NO-PROMOTION` without making coroutine
  support a prerequisite.
- [x] M7-G0: audit gateway `0a8fe1e` and independent consumer `b525416`; record
  external implementation `DEFER` and shared RPC `NO-PROMOTION` at
  `44493b1d37c16567990e1660153d6b0843a8eecc` because no common gateway RPC
  wire/lifecycle contract existed at readiness time.
- [x] M7-G0b: bind clean gateway M4 closure `d03cacd` and external M7 governance
  `92a2607`; set the gateway callback/value adapter to `RESUME` while retaining shared
  RPC `NO-PROMOTION` and deferred Core intent.
- [x] M7-G0c: bind gateway implementation `e43393c` and closure `588acd0`, exact
  dual-platform/sanitizer/repeat/fuzz evidence, and YanGameServer `b525416` RPC
  8/8 revalidation; field comparison closes shared RPC as `NO-PROMOTION` and
  advances the front to M8 without a Core RPC/Lua surface or empty v0.6 release.
- [x] M8: compare the current callback-based Core and gateway with the
  ActorScheduler-bound independent coroutine consumer; Core EventLoop/
  TimerQueue 3/3 and exact YanGame async/coroutine/timer/RPC/persistence 9/9
  prove different result, executor, frame, resume, timer, and retirement
  contracts. Close as `NO-PROMOTION`, keep all six async intents deferred, add
  no coroutine/awaiter surface or empty v0.7 release, and advance to M9.
- [x] IOE-X1–X9: the default-off, Linux-only, non-installed Engine through
  listener/Accept vertical slices are closed with exact-commit evidence.
- [x] IOE-X10: fixed 256-route epoll/io_uring listener comparison is `PROMOTE`
  only for later source-private shaping; raw evidence is bound to `f5d39b8`.
- [x] M2: select the exact promotion commit and complete dual-platform CI,
  sanitizers, consumers, performance/capacity/fault, 1h/3h endurance,
  package, SBOM, and notices before naming the internal candidate.
  - [x] Candidate `0c30124`: main CI `32575344030/1`, candidate/dedicated
    capacity `32575517520/1` and `32575519054/1`, paired benchmark
    `32575521055/1`, and repeat-50 `32575522678/1` passed.
  - [x] Endurance: candidate-1h `32576118286/1` and release-3h
    `32580658838/1` passed without waiver; same-commit pair and release
    promotion independently revalidated.
  - [x] Internal source/binary packages, SPDX 2.3 SBOM, notices,
    `SHA256SUMS`, and the 723-file complete evidence index passed verification.
