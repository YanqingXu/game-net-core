# Roadmap

game-net-core is the component-split migration target for `mini_trantor`.
The roadmap keeps that migration staged so the networking core becomes stable
before protocol, transport, game-foundation, or experimental modules are added.
See `migration_status.md` for the current checked state of these phases.

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

Completed in the current worktree:

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

The Phase 4 foundation and the audit remediation are implemented locally with
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

The frozen `final-v4` dirty worktree passes 85/85 configured tests in Windows
Debug (36.47 seconds), Windows Release (36.97 seconds), and Linux Clang Release
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
The Linux and Windows Release install consumers each pass 1/1. Both platforms
also produce all three fixed Release Phase 4 benchmark scenarios with
`status: ok` and pass the shared validator. Every `final-v4` result remains
dirty-worktree local preflight, not immutable remote same-SHA evidence.

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
Windows/IOCP result from the same run with identical scenario parameters. These
contracts are implemented locally, but no immutable Phase 4 candidate has been
committed or pushed and none has a current-candidate remote same-SHA run.

Draft PR #4 head `0d62054e148a1c95793799eb88856363ac6843d3` and five-job `ci`
run `29147391402` (#32) are retained only as pre-hardening history. They do not
validate the current remediation changes.

Phase 4 remains evidence-gated rather than implementation-blocked. Before a
preview tag, commit and push an immutable candidate and record all six same-SHA CI
producer artifacts plus their aggregate evidence set, Linux TSan and actual
libFuzzer evidence, structured repeat-50 threading plus Pipeline/Broadcast soak
manifests, the same-run Linux/Windows Phase 4 benchmark pair manifest, and a
formal preview release. Draft PR #4 still points at the older pre-hardening
candidate; it has not been updated to the current worktree. Experimental HTTP,
RPC, UDP/KCP, TLS, and coroutine work remains deferred behind separate intent
promotion.
