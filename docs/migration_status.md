# Migration Status

Historical audit field preserved by contract — Last checked: 2026-07-11

Phase 4 Preview publication checked: 2026-07-12

## Current Task Goal

`game-net-core` is the component-split migration target for the larger
`mini_trantor` project.

The Reactor / TCP foundation remains frozen at `v0.1.0-core-preview`. Phase 4
protocol, transport, session, logic-loop, pipeline-example, and broadcast
foundations are now implemented as one-way upper layers. Experimental
UDP/KCP/TLS/coroutine and HTTP/WebSocket/RPC adapters remain deferred.

The post-Preview production-hardening line is now active. Its first completed
contracts suppress Linux per-write `SIGPIPE`, bound per-connection admitted
output bytes across owner/cross-thread sends, return explicit overload results,
cap unread input buffers, apply owner-loop high/low-water read throttling, and
publish completion-aware graceful server drain results with a forced timeout fallback.
Recoverable listener/connection socket setup now returns explicit errors, and
Acceptor/TcpServer expose an owner-loop Retry-or-Stop runtime error policy.
Asynchronous EventLoop callbacks now have observable Continue/Quit exception
containment, thread-init failures return through the startup handshake, and a
throwing TCP business callback closes only its offending connection while
server bookkeeping and later admission continue.
TcpServer now optionally enforces global/per-peer connection limits, a bounded
per-peer fixed-window attempt rate, and base-loop unauthenticated deadlines;
all are disabled by default and expose distinct cumulative metrics.

Phase 6 production-candidate work is now active. The first gate defines the
0.3 source-compatibility boundary with a versioned installed-header/target
manifest: stable Core declarations are fingerprinted, Phase 4 APIs remain
provisional, platform backend headers are unsupported interfaces, and ABI
compatibility remains explicitly out of scope before 1.0.
MetricsExporter and its producer adapters are now implemented, and the fixed
Core/Phase-4 Release matrix has a reviewed same-runner regression budget,
three-repetition medians, and recursively hashed raw-sample evidence. A local
Windows/IOCP preflight passed all 12 scenarios; it validates the tooling but is
not a substitute for same-commit retained Linux/Windows release evidence.

## Phase Status

| Phase | Scope | Current status |
| --- | --- | --- |
| 1 | Initialize the `game-net-core` project skeleton | Present: top-level CMake, README, AGENTS, docs, intents, rules, include/src/tests/examples layout |
| 2 | Migrate Reactor / TCP core | Present: base utilities, socket helpers, Channel/Poller/EventLoop/TimerQueue, Acceptor/Connector, TcpConnection/TcpServer/TcpClient |
| 3 | Split CMake targets and test structure | Present: `gamenet_core`, `GameNet::core`, install/export package config, echo examples, unit/contract/integration test directories, scope/intent/documentation guards, install consumer fixture, an opt-in core benchmark target, and Acceptor/Buffer/Channel/Connector/InetAddress/Poller/Socket/TcpClient/TcpServer/TcpConnection/EventLoopThread/EventLoopThreadPool contract tests |
| 4 | Gradually migrate protocol / transport / game foundation / experimental | Foundation merged and published as `v0.2.0-phase4-preview`: PacketFramer, TransportEndpoint/TCP adapter, PlayerSession/SessionManager, bounded LogicLoop queue, pipeline demo/integration, and broadcast/backpressure; experimental transports remain deferred |
| 5 | Production hardening | Implementation complete and frozen candidate `be749adc4bce7e1771b84c77c42bf080625805e9` validated: Linux peer-close writes no longer inherit process-terminating `SIGPIPE`; connection input/output admission has finite hard limits plus high/low-water read throttling; EventLoop admission and per-iteration drain are bounded; graceful server drain is completion-aware with timeout force-close; recoverable listener/connection setup errors use explicit results and accept Retry/Stop policy; asynchronous callback exceptions are contained and connection-local business failures preserve server availability; TcpServer has optional global/per-peer connection caps, bounded fixed-window attempt limiting, and unauthenticated deadlines |
| 6 | Production candidate | In progress: active release intent, 0.3 compatibility policy, exact installed public-header/target inventory, stable Core declaration fingerprints, metrics export, and the same-runner performance regression gate are implemented; fault injection, 24/72-hour endurance, and frozen-candidate release evidence remain gated |

## Production-Hardening Worktree State

- Intent semantics resolve 29 active targets and 89 explicit verification
  paths, with `connection_backpressure_controller` and `graceful_shutdown`
  promoted from deferred design assets to active `GameNet::core`
  implementation authority.
- MetricsExporter is promoted to active Core authority with thread-safe
  in-memory aggregation, immutable static labels, deterministic Prometheus
  snapshots, exception-contained Core/Logic/Broadcast recorder adapters, and
  bounded-cardinality metric names. Its unit, concurrency/owner-loop contract,
  and real LogicLoop/Broadcast integration tests pass in the 88-test Debug
  inventory.
- The Release performance gate builds the reviewed baseline and candidate on
  the same runner, retains 72 raw samples per platform, compares medians for
  12 fixed Core/Phase-4 scenarios, and rejects parameter, hash, baseline, or
  budget drift. The local Windows/IOCP preflight passed 12/12 comparisons;
  authoritative release evidence remains bound to the later frozen commit.
- `tests/contract/socket/test_socket_contract.cpp` carries the Linux child-
  process/default-SIGPIPE contract.
- `tests/contract/tcp_connection/test_tcp_connection_high_water_mark.cpp`
  verifies hard-limit rejection plus read pause/resume hysteresis, and
  `test_tcp_connection_cross_thread_send.cpp` verifies pre-owner-loop byte
  reservation and overload rejection.
- `tests/contract/buffer/test_buffer_contract.cpp` verifies exact per-read caps;
  `test_tcp_connection_send_after_close.cpp` verifies unconsumed input reaches
  its configured cap and closes without further Buffer growth.
- `tests/contract/event_loop/test_event_loop.cpp` verifies normal-capacity
  rejection, explicit reserve exhaustion, accepted-work preservation, and a
  timer interleaving between bounded functor batches.
- `tests/contract/tcp_server/test_tcp_server_stop_active_write.cpp` verifies
  natural output drain, repeated shared completion, timeout force-close with
  exact counts, and immediate-stop compatibility.
- `tests/contract/socket/test_socket_contract.cpp` verifies invalid socket
  creation/bind/listen remain non-fatal, and
  `tests/contract/acceptor/test_acceptor_contract.cpp` verifies unavailable
  listener bind is reported by exception while the normal path leaves the
  error policy untouched.
- `tests/contract/event_loop/test_event_loop.cpp` verifies Channel, Timer,
  pending-functor, and metric exception sources, Continue/Quit behavior,
  throwing-policy containment, thread-init propagation, and partial worker-pool
  startup rollback.
- `tests/contract/tcp_server/test_tcp_server_contract.cpp` verifies message and
  disconnect callback exceptions close only the offending connection, observer
  exceptions remain contained, cleanup reaches zero connections, and a later
  client is still served. `tests/contract/connector/test_connector_contract.cpp`
  verifies a throwing diagnostic observer cannot interrupt connect success.
- The same TcpServer contract uses real clients to verify global and per-peer
  limits, bounded fixed-window rate rejection, cross-worker authentication,
  authentication timer cancellation, unauthenticated close, exact counters,
  base-loop metric affinity, and metric-callback exception containment.
- Windows MSVC Debug passes all 85/85 configured tests in 40.69 seconds after
  the recoverable socket/accept, callback-containment, and TcpServer admission
  changes; the focused admission contract also passes 20 consecutive runs.
- Production-hardening worktree Windows MSVC Debug AddressSanitizer passes
  85/85 in 47.38 seconds. CTest now supplies the selected compiler's ASan
  runtime directory per test, and the generated executable imports
  `clang_rt.asan_dynamic-x86_64.dll` without a caller-side `PATH` mutation.
- A clean Windows MSVC Release tree passes 85/85 in 38.45 seconds; its installed
  `GameNetCore 0.2.0` package configures, builds, and runs the external
  `find_package` consumer 1/1.
- The production-hardening worktree repeat-50 gates pass 3,050/3,050 threading
  executions in 1,716.27 seconds and 400/400 Pipeline/Broadcast executions in
  37.88 seconds. Both repository evidence verifiers accept the exact inventory,
  labels, repeat count, timeout, command, and result logs.
- All seven fixed Windows Release IOCP core/Phase-4 benchmark scenarios return
  their expected schema, platform/backend/build identity, and `status: ok`.
- Frozen production-hardening candidate
  `be749adc4bce7e1771b84c77c42bf080625805e9` is pushed on
  `codex/production-hardening`. Workflow-dispatch `ci` run `29791363106`
  passed all six producers and the aggregate evidence gate: Linux CMake,
  ASan/UBSan with 1,000 libFuzzer executions, TSan over all 61 threading
  tests, Linux Release, Windows Debug IOCP, and Windows Release IOCP. Each
  full-test producer executed 85/85 tests, and Linux plus both Windows package
  consumers executed 1/1.
- The same candidate owns successful `long-soak` run `29791364648`: repository
  verifiers independently accept 3,050/3,050 threading executions in 1,690.82
  seconds and 400/400 Pipeline/Broadcast executions in 34.19 seconds, with
  `repeat=50` and per-test `timeout=60`.
- The same candidate owns successful `core-benchmark` run `29791365828`.
  All eight fixed Core Release scenarios report `status: ok`, and the paired
  Phase 4 evidence gate verifies the three fixed scenarios on both Linux/epoll
  and Windows/IOCP with identical parameters. All remote sanitizer, package,
  long-soak, and benchmark evidence is bound to one frozen commit; Windows
  results are not used as a substitute for Linux execution.

## Verification State

The current worktree configures 88 configured CTest tests: 8 unit tests, 73 contract tests, and 7 integration tests. Phase 4 coverage now includes bounded
PacketFramer/real-fuzz contracts, transport/session/logic lifecycle and race
contracts, four Pipeline integrations, and four Broadcast contracts/integrations.

Local Phase 4 hardening `final-v4` preflight subsequently frozen into candidate
`5ebad2c1a4a9487437340935e21f7468140c7e8d`:

- Windows MSVC Debug: 85/85 configured tests passed in 36.47 seconds.
- Windows MSVC Release: 85/85 configured tests passed in 36.97 seconds.
- Linux Clang 19 Release: 85/85 configured tests passed in 34.76 seconds.
- All 27 Python scope, intent, CI, and CMake guards passed locally.
- PacketFramer contracts and deterministic round-trip harness compile under GNU
  g++ C++23 with `-Wall -Wextra -Wpedantic -Werror`. The real libFuzzer target
  also completed exactly 1000 local Clang 19 ASan/UBSan runs with 6 binary
  seeds, its dictionary, fixed seed `424242`, `max_len=8192`, and a 2-second
  input timeout. The command did not use `max_total_time`; the saved mutated
  corpus contains 90 files and no crash artifact. Candidate main-CI run
  `29160903594` repeated the sanitizer-backed fuzz gate successfully and
  retained its SHA-bound log, corpus, dictionary, and artifact evidence.
- The current Debug inventory includes 63 threading-labeled tests and eight
  Pipeline/Broadcast tests. On the latest tree, the complete threading slice
  passed repeat 50: 61 x 50 = 3,050 executions with zero failures in 1,777.76
  seconds. The focused Pipeline/Broadcast slice also passed repeat 50: 8 x 50 =
  400 executions with zero failures in 54.16 seconds. Both
  `gamenet.ctest_repeat_evidence.v1` manifests report `result: success` and bind
  to the same `final-v4` inventory SHA-256
  `37ee7fb3572c911fa771ba42ce1fcb91a252bc2c78c56b98b280f5305c77a09a`.
  Candidate `long-soak` run `29161167423` then completed the same exact
  selections remotely: 3,050/3,050 threading and 400/400 Pipeline/Broadcast
  executions, with both structured verifiers, the job manifest, and artifact
  upload successful.
- Release install/export for package version `0.2.0` installs
  `GameNet::core`, `GameNet::protocol`, `GameNet::transport`,
  `GameNet::game_session`, `GameNet::game_logic`, and `GameNet::broadcast`.
  Clean Linux Clang and Windows MSVC Release external consumers discovered the
  exact version with `find_package(GameNetCore 0.2.0 EXACT)`, linked all six
  targets, exercised transport/session API, and each passed executable CTest
  1/1. In `final-v4`, the Linux consumer completed 1/1 in 0.02 seconds and the
  Windows consumer reported a 0.02-second test case (0.03 seconds total CTest
  time).
- A full Windows MSVC Debug AddressSanitizer checkpoint passed all 85/85 CTests
  in 43.19 seconds.
  Its first full run exposed a Connector ConnectEx timeout/cancel late-
  completion Channel use-after-free; Poller-retained operation state and
  completion-drain lifetime fixed it before the green rerun.
- Linux Clang 19 ASan/UBSan also passed the full current 85/85 CTest inventory
  in 36.18 seconds in the Docker validation environment.
- Linux Clang 19 TSan passed the complete 61/61 `threading` inventory after a
  genuine test-fixture race was fixed: the repeating TimerQueue cancellation
  handle is now initialized, read, and canceled on its owner loop. Docker's
  default seccomp had to be relaxed for TSan's `ADDR_NO_RANDOMIZE` personality
  syscall; the container was not run privileged. The `final-v4` threading run
  completed in 35.63 seconds.
- The `final-v4` Linux Clang/epoll and Windows MSVC/IOCP Release Phase 4 benchmark
  executables each report `status: ok` for framing, logic-queue, and broadcast-
  fanout. Both raw three-file sets pass the shared semantic/count-invariant
  validator. They are local snapshots from different execution environments,
  not performance thresholds or direct cross-host scores. Candidate benchmark
  run `29161168417` subsequently completed both remote producers and the
  aggregation-only `gamenet.phase4_benchmark_pair_evidence.v1` gate successfully.
  Linux framing/logic-queue/broadcast-fanout recorded 9444.046 MiB/s,
  452934.439 ops/s, and 5036891.294 ops/s; Windows recorded 2575.474 MiB/s,
  32475.926 ops/s, and 4535488.872 ops/s respectively.
- Pipeline lifecycle tests now prove that a strong callback-state reference does
  not preserve execution permission after atomic revocation. They cover
  synchronous `stop()` from a Logic-stage callback when management and logic
  share one loop, an endpoint observer spanning revocation with no later send,
  and one exact frame batch containing authentication plus an early command.
  Session lifecycle coverage keeps live heartbeat and offline producers
  overlapping the management-loop drain and uses per-producer sentinels to prove
  final Offline state and empty indexes only after all posts drain.
- Intent semantics now resolve all 25 active targets and 74 explicit
  verification paths. The 16 frozen Core library intents retain only their
  documented metadata exemption; seven enriched Phase 4 intents still require
  artifact kind, provenance, and non-empty verification. The dependency guard
  derives the six production library edges from an actual configured CMake
  target graph and rejects direct and transitive reverse dependencies through
  negative fixtures.
- Clean-checkout CI no longer relies on the ignored local `mini_trantor/` tree:
  all semantic-guard jobs checkout `YanqingXu/mini_trantor` at immutable commit
  `3eba368475a68f677aae920d4f299b155db23d57`, then verify 20 declared source
  paths directly against that Git object before intent semantics run.
- The repository now configures six CI producer jobs: Linux Debug, Linux
  ASan/UBSan plus real fuzz, Linux TSan, Linux Release, Windows Debug IOCP, and
  Windows Release. Each producer locks the 85-test inventory (TSan also locks
  `threading=61`), executed consumers lock one test, and 90-day evidence bundles
  contain JUnit/raw logs plus a SHA/run/attempt-bound `gamenet.ci_evidence.v1`
  manifest; ASan also preserves the fuzz dictionary, mutated corpus, log, and
  artifacts. A seventh aggregation-only gate validates exact job identities,
  selected/executed tests, shared candidate/run identity, bytes, and hashes, then
  emits `gamenet.ci_evidence_set.v1`. It is not a seventh platform producer.
  The manual long-soak workflow emits `gamenet.ctest_repeat_evidence.v1` for the
  exact selected tests and repeat count, while the two Phase 4 benchmark
  producers feed an aggregation-only pair gate that emits
  `gamenet.phase4_benchmark_pair_evidence.v1` for one Linux/epoll and one
  Windows/IOCP result with identical scenario parameters.
  Functional candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d` is committed and
  pushed, and was the PR #4 head when candidate evidence was produced.
  Pull-request `ci` run
  `29160903594` checked GitHub merge-ref
  `e461b597f2642e000717f536f3b430b804ba26ad`, bound candidate and PR-head
  identity to `5ebad2c1a4a9487437340935e21f7468140c7e8d`, and passed all six
  producers plus the aggregate gate, 7/7. The same candidate owns successful
  `long-soak` run `29161167423` and paired benchmark run `29161168417`.
  Final PR-head run `29162961320` passed 7/7 before PR #4 was owner-authorized
  and merged with merge commit `7668d6b82a0d815ccd79f83c572bc0a36bcceea0`.
  Main push run `29168786199` validated that exact commit with six producers
  plus aggregate, 7/7. Annotated tag `v0.2.0-phase4-preview` (tag object
  `b76077f839230fb99f5e570ef623174747f04249`) and the
  [formal GitHub prerelease](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview)
  are published. PR #4 had zero submitted GitHub reviews; that process
  limitation is retained rather than described as completed review.

Pre-hardening Phase 4 baseline retained as immutable historical evidence:

- Windows MSVC Debug: 74/74 passed in 34.67 seconds.
- Windows MSVC Release: 74/74 passed in 34.41 seconds.
- PacketFramer contract and deterministic round-trip smoke also compiled and ran with GNU g++ C++23
  under `-Wall -Wextra -Wpedantic -Werror`.
- Windows MSVC Debug threading slice: 51/51 passed across repeat-5 (255 test
  executions) in 170.00 seconds with a 30-second per-test timeout.
- The six exported targets were installed, discovered, linked, and executed by
  the downstream install consumer.
- Pre-hardening PR #4 head `0d62054e148a1c95793799eb88856363ac6843d3`
  was associated with five-job `ci` run `29147391402` (#32) on 2026-07-11;
  that run actually checked out and tested GitHub merge-ref
  `31107e8964a0f206087fe2f029a39a15107f6bda`, not the head object itself.
- The TSan job in that historical run selected the then-current `threading`
  label inventory and passed 51/51 tests. That count belongs to the old
  74-test tree; it is not the current 61-test threading inventory.
- That historical run predates the current remediation and did not include the
  sixth Windows Release job, real libFuzzer execution, repeat-50 Phase 4 soak,
  Phase 4 benchmark artifacts, or formal preview-release evidence. It cannot
  certify candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d`. The immutable
  Phase 3.5 Core Preview evidence remains recorded separately below.

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON`
- Build: `cmake --build build --parallel`
- Test: `ctest --test-dir build --output-on-failure`
- Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`
  (this fixed label belongs to the Phase 3.5 release record; the verified
  Phase 4 functional candidate and runs are recorded above).
- CI workflow run id: `29079836593` (`ci` #29, `main`).
- Validation date: 2026-07-10.
- Result: all five jobs passed:
  `Linux CMake build and tests`, `Linux ASan/UBSan build and tests`,
  `Linux TSan race-oriented build and tests`, `Linux Release build`, and
  `Windows MSVC IOCP build and tests`.
- Release: annotated tag `v0.1.0-core-preview` peels to the validated commit.
  Its tag-only release record and known limitations are documented in
  `docs/development/releases/v0.1.0-core-preview.md`; no GitHub Release assets
  or historical checksum manifest existed.
- Focused candidate commit `a7fd77cbd2140041cebb3f900d5c609fafc2adad`
  passed PR `ci` run `29076601085` (#27) and is preserved as a parent of the
  release merge commit. It owns the same-SHA long-soak and benchmark evidence
  recorded below; merge commit `c4818d4` combines it with evidence documentation
  without rewriting either commit.
- The preceding audited candidate, commit
  `d1474b5f32e609a7d2e2648af31b45635595d304`, failed in `ci` run id
  `29073362905` (#26) on
  `contract.tcp_client.test_tcp_client_repeated_connect` at
  `serverConnectedCount == 1`. Linux synchronously completed the first
  connection teardown before queued duplicate `connect()` submissions were
  drained, allowing a second Connector attempt. The validated fix
  coalesces one generation-tagged connect request across each pending/active
  lifecycle and releases it on terminal no-retry failure or connection
  removal.
- Race-oriented CI: this worktree adds a `linux-tsan` workflow job named
  `Linux TSan race-oriented build and tests`. It configures with
  `GAMENET_ENABLE_TSAN=ON`, builds the Debug target set, and runs the CTest
  suite filtered to the `threading` label. That label includes cross-thread
  APIs, worker-loop scheduling, pending read/write forceClose cancel-close
  contracts, direct Connector retry-stop cancellation contracts,
  mixed-timing pending ConnectEx stop contracts,
  active retry-enabled TcpClient stop-after-peer-close contracts,
  active cross-thread TcpClient disconnect contracts,
  repeated active TcpClient disconnect idempotence contracts,
  repeated active TcpClient stop idempotence contracts,
  repeated active TcpClient connect idempotence contracts,
  active cross-thread TcpClient connect contracts,
  active cross-thread TcpClient retry configuration contracts,
  post-close TcpConnection send ignore contracts, mixed-timing pending-read forceClose contracts,
  mixed-timing pending-write forceClose contracts,
  repeated TcpConnection shutdown idempotence contracts,
  worker-owned active-write stop contracts, worker-callback TcpServer stop
  contracts, and repeated TcpServer stop idempotence contracts.
  Phase 4 candidate race-oriented remote evidence is the successful Linux TSan
  producer in `ci` run `29160903594`. Latest recorded race-oriented CI remote green evidence is `ci` #29 on release commit
  `c4818d4b3956c85830e04d4a1f32df4ad701d453` only within the preserved Phase
  3.5 Core history; it is not the current Phase 4 evidence.
- Scope guard: local self-test and repository scan pass; CI runs both before
  CMake configure.
- Intent/documentation guards: CI runs the intent consistency guard, intent metadata contract guard, Core benchmark contract guard, Logger thread-contract guard, EventLoop contract guard, TCP lifecycle contract guard, TcpConnection context contract guard, TcpConnection thread-contract guard, EventLoopThreadPool contract guard, TimerQueue contract guard, threading gate contract guard, migration status contract guard, install/package contract guard, MSVC UTF-8 build contract guard, platform backend contract guard, Windows IOCP milestone contract guard, Windows IOCP data-path contract guard, sanitizer flag contract guard, Release-safe test guard, and workflow job structure guard before CMake configure. The EventLoop contract guard now also requires the cross-thread-observed pending functor execution state to be atomic or synchronized.
- Intent governance: all 60 formal `*.intent.md` documents now carry ordered
  `status`, `target`, `migration_source`, and `promote_gate` front matter and
  appear exactly once in the intent index: 25 active contracts, 23 deferred design assets, and 11 legacy source-project stage documents.
  Active bodies reject stale `MINI_ENABLE_*`, `mini::`, `mini/net`, and
  `mini-trantor` contracts. Deferred bodies require an explicit future gate;
  legacy v1/v2/v3/v5/v6 and M1-M32 documents target `historical` with
  `promote_gate: never`, so their old options, test counts, and phase claims are
  not current repository evidence. The semantic guard dynamically resolves all
  25 active targets and their 74 explicit verification paths: 69 regular C++
  CTest sources, one libFuzzer target, and four Python governance tests executed
  by the workflow. Six new active Phase 4 component/use-case intents target
  protocol, transport, game_session, game_logic, broadcast, and the pipeline
  example; a seventh active Phase 4 intent defines the opt-in performance
  baseline, and the echo use case remains an active example contract. The 16
  frozen Core library intents must still resolve to the real installed
  `GameNet::core` target but retain the documented artifact-kind, provenance,
  and non-empty-verification metadata exemption. Production dependency
  direction is checked from a configured CMake target graph, including
  transitive reachability and direct/transitive reverse-dependency fixtures.
- Added lifecycle and base coverage in this worktree: coost-compatible Logger unit and contract coverage, concurrent Logger runtime-configuration coverage (`contract.base.test_logger_thread_safety`), EventLoop cross-thread pending-functor execution-state atomicity guard, cross-thread TcpConnection state observation (`contract.tcp_connection.test_tcp_connection_cross_thread_state`), Connector completed-Channel member release guarded by the repeated-connect contract, server stop with active connections, server stop during active write, server stop soak for worker-owned connections, server multi-worker stop from the base loop, server worker-owned active-write stop, server worker-callback TcpServer stop soak, server repeated stop idempotence (`contract.tcp_server.test_tcp_server_repeated_stop`), client retry stop race, client retry-stop soak, direct Connector retry-stop cancellation (`contract.connector.test_connector_retry_stop`), client stop during pending ConnectEx, client pending ConnectEx stop soak, client cross-thread stop during pending ConnectEx, client mixed-timing pending ConnectEx stop soak, client destruction during pending ConnectEx, client destruction with active TcpConnection, client mixed-timing active-connection stop soak, client cross-thread active disconnect, client repeated active disconnect idempotence, client repeated active stop idempotence (`contract.tcp_client.test_tcp_client_repeated_stop`), client repeated active connect idempotence (`contract.tcp_client.test_tcp_client_repeated_connect`), client cross-thread active connect, client cross-thread retry configuration (`contract.tcp_client.test_tcp_client_cross_thread_retry_config`), peer close convergence, peer reset convergence, error-triggered teardown idempotence, cross-thread send delivery, post-close TcpConnection send ignore (`contract.tcp_connection.test_tcp_connection_send_after_close`), write-complete callback ordering, shutdown while output pending, cross-thread shutdown draining, repeated TcpConnection shutdown idempotence (`contract.tcp_connection.test_tcp_connection_repeated_shutdown`), high-water mark notification, repeated forceClose idempotence, repeated connectDestroyed stale-registration cleanup (`contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed`), cross-thread forceClose soak, cross-thread pending-read forceClose, cross-thread pending-write forceClose, pending-read forceClose cancellation before connection destruction, mixed-timing pending-read forceClose soak, pending-write forceClose soak before connection destruction, mixed-timing pending-write forceClose soak, TimerQueue ready-timer cancellation race coverage, EventLoopThreadPool queued-work soak coverage, and EventLoopThreadPool restart-stop soak coverage.
- The repeated-connect contract now also covers generation-gated request
  admission and a fresh explicit connect after terminal no-retry failure.
- Test support hardening: repeated TcpConnection lifecycle/race setup now uses
  shared tests/support helpers. `SocketPair.h` centralizes socketpair,
  nonblocking, and small-send-buffer setup; `ClientSocket.h` centralizes nonblocking test-client connect and cleanup
  for TcpServer lifecycle contracts, Acceptor/Socket contracts, and
  TcpConnection peer-reset setup; `TcpConnectionHarness.h`
  centralizes loop-bound TcpConnection construction from connected socketpair
  fixtures; `TcpConnectionCallbacks.h`
  centralizes owner-loop connection/disconnection/close callback counting for
  force-close contracts; `LoopTest.h` centralizes EventLoop watchdog execution
  for TcpConnection force-close contracts, TcpClient lifecycle watchdogs, and
  TcpServer lifecycle watchdogs;
  `ThreadHandoff.h` centralizes one-shot and delayed non-owner-thread handoff
  for cross-thread lifecycle contracts; `FutureTest.h` centralizes bounded future waits for EventLoop, EventLoopThread, TimerQueue, and EventLoopThreadPool async contract tests; and
  `TcpClientStopHarness.h` centralizes retry-stop stale-reconnect assertions
  for client lifecycle contracts; `TcpServerHarness.h` centralizes multi-client TcpServer connection setup and worker-loop distribution assertions
  for server lifecycle/race contracts.
- Sanitizers: CI includes an ASan/UBSan Debug build and CTest job for the
  Reactor / TCP foundation. The worktree also defines a Linux TSan
  race-oriented threading test gate for thread-affinity and lifecycle risks.
- Long soak: this worktree adds a non-default `long-soak` workflow. It is
  manual-only through `workflow_dispatch` and runs the `threading` CTest slice
  with `ctest --repeat until-fail` so mixed-timing lifecycle contracts can
  gather stronger soak evidence without blocking ordinary push or pull-request
  CI. The long-soak repository guard parity includes the EventLoop contract guard,
  keeping manual soak guards aligned with the ordinary CI guard surface. The
  current workflow input defaults to repeat 50 with a 60-second per-test
  timeout. The workflow locks the 85-test inventory, `threading=61`,
  `game_pipeline=4`, and `broadcast=4`, then verifies the raw result lines for
  every selected test and exact repeat count. Its two
  `gamenet.ctest_repeat_evidence.v1` summaries include per-test executions,
  elapsed time, command, inventory hash, and raw-log hash; the enclosing
  canonical SHA/run/attempt artifact also carries `gamenet.ci_evidence.v1`.
  Local Windows Debug long-soak evidence also covers the previous
  43-test threading slice before the cross-thread TcpClient retry configuration
  contract expanded the threading label to 44 tests. That earlier local run used:
  `ctest --test-dir build -C Debug --output-on-failure -L threading --repeat until-fail:20 --timeout 60`;
  43/43 threading-labeled tests passed across 20 repeats on 2026-07-09,
  and CTest reported total test time was 637.56 seconds. The then-expanded
  44-test threading slice was covered once by the full Windows Debug and Release
  CTest checkpoint. The new cross-thread TcpConnection state contract expands the
  threading slice to 45 tests. The Logger concurrency contract expanded the
  then-current threading slice to 46 tests.
  The 2026-07-10 Windows Debug preflight passed all 46 threading tests across 5
  repeats with `--timeout 15`; CTest reported 176.90 seconds on 2026-07-10.
  The repaired repeated-connect contract separately passes 50 repeats in
  32.41 seconds. The corresponding historical remote GitHub `long-soak`
  evidence is recorded in run
  `29077148022`, job `86311227712`, commit
  `a7fd77cbd2140041cebb3f900d5c609fafc2adad`, repeat 50, timeout 60 seconds,
  completed successfully at 2026-07-10T08:04:12Z. The log records
  `ctest --test-dir build-long-soak --output-on-failure -L threading --repeat until-fail:50 --timeout 60`;
  46/46 threading-labeled tests passed in 1632.47 seconds of real CTest time,
  and the complete job took 28m27s. Historical run `28986707243` covered the
  earlier 36-test slice at repeat 20 on commit
  `9b27a0a3c3993cb1f90ef4357fa80027205ca221`.
- Release: CI includes a Release build and CTest gate for the Reactor / TCP
  foundation. C++ tests use a Release-safe helper instead of standard `assert`
  so contract checks remain active with `NDEBUG`. Local Windows Release
  evidence now also passes after shortening generated CMake test target names
  to keep MSBuild `.tlog` paths below Windows path-length limits:
  `cmake --build build-release --config Release --parallel` succeeds, and
  `ctest --test-dir build-release -C Release --output-on-failure --timeout 10`
  reports 67/67 Release tests passed for the then-current Core worktree in 37.38 seconds
  on 2026-07-10.
- Core benchmark: `GAMENET_BUILD_BENCHMARKS` defaults to `OFF`; when enabled it
  builds the non-installed, non-CTest `gamenet_core_benchmark` executable. Its
  `gamenet.core_benchmark.v1` JSON covers echo throughput/P50/P99, one-versus-two
  worker scaling, idle-connection process working set, and slow-client output
  accumulation while identifying backend and completion mode. Local Windows
  MSVC Release evidence on 2026-07-10 records 51.979 MiB/s and P99 61.7 us with
  one worker, 73.994 MiB/s and P99 59.2 us with two workers, about 71,264 bytes
  of process working-set delta per idle connection at 256 connections, and a
  67,465,216-byte working-set increase after offering 8 MiB to each of four
  non-reading clients. The slow-client run fired four high-water callbacks and
  explicitly reports `high_water_notification_only`; it is not a memory-cap
  claim. Raw local evidence is under
  `docs/development/benchmark_results/2026-07-10-windows-msvc-release/`.
  The manual-only `core-benchmark` workflow run `29077151229` completed successfully on
  `a7fd77cbd2140041cebb3f900d5c609fafc2adad` and published matching Linux
  epoll (`epoll_wait_batch`) and Windows IOCP
  (`single_get_queued_completion_status`) Release artifacts. Both artifact
  names include the full commit SHA and each contains the same four scenarios.
  Linux one/two-worker echo measured 32.337/65.234 MiB/s with P99
  97.419/74.229 us; Windows measured 16.188/26.110 MiB/s with P99
  162.8/151.3 us. The 256-connection working-set deltas were 991,232 bytes on
  Linux and 18,137,088 bytes on Windows. Four slow clients offered 8 MiB each,
  producing four high-water callbacks and working-set deltas of 26,562,560
  bytes on Linux and 67,493,888 bytes on Windows. All eight JSON files use
  `gamenet.core_benchmark.v1` and report `status: ok`; these values are evidence
  snapshots, not performance thresholds.
- Install/package: CI installs the core target and builds an external consumer
  fixture through `find_package(GameNetCore)` and `GameNet::core`. Release
  install/package consumer also passes locally: the Release build installs to
  `build-release/_install`, the external fixture configures into
  `build-release-install-consumer`, builds in Release, and the
  `gamenet_install_consumer` executable exits 0.
- Windows: Windows support is now represented by a `windows-msvc` workflow job
  for the Reactor / TCP IOCP backend. Local VS2026 Debug configure/build
  succeeds after the MSVC `/utf-8` and `/FS` compile options were added, and a
  local full Windows Debug CTest run with a 10-second per-test timeout passes
  67/67 configured tests with 0 failing tests in 43.57 seconds on 2026-07-10.
  The IOCP data path now covers
  `contract.event_loop.test_event_loop`,
  `unit.base.test_logger`,
  `contract.base.test_logger_contract`,
  `contract.base.test_logger_thread_safety`,
  `contract.event_loop_thread_pool.test_event_loop_thread_pool`,
  `contract.timer_queue.test_timer_queue`,
  `contract.acceptor.test_acceptor_contract`,
  `contract.connector.test_connector_contract`,
  `contract.connector.test_connector_retry_stop`,
  `contract.poller.test_poller_contract`,
  `contract.tcp_client.test_tcp_client_contract`,
  `contract.tcp_client.test_tcp_client_retry_stop_soak`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect_soak`,
  `contract.tcp_client.test_tcp_client_cross_thread_stop_pending_connect`,
  `contract.tcp_client.test_tcp_client_stop_pending_connect_mixed_timing_soak`,
  `contract.tcp_client.test_tcp_client_destroy_pending_connect`,
  `contract.tcp_client.test_tcp_client_destroy_active_connection`,
  `contract.tcp_client.test_tcp_client_stop_active_connection_mixed_timing_soak`,
  `contract.tcp_client.test_tcp_client_cross_thread_disconnect_active`,
  `contract.tcp_client.test_tcp_client_repeated_disconnect`,
  `contract.tcp_client.test_tcp_client_repeated_stop`,
  `contract.tcp_client.test_tcp_client_repeated_connect`,
  `contract.tcp_client.test_tcp_client_cross_thread_connect`,
  `contract.tcp_client.test_tcp_client_cross_thread_retry_config`,
  `contract.tcp_server.test_tcp_server_contract`,
  `contract.tcp_server.test_tcp_server_stop_active_connections`,
  `contract.tcp_server.test_tcp_server_stop_active_write`,
  `contract.tcp_server.test_tcp_server_stop_multi_worker`,
  `contract.tcp_server.test_tcp_server_stop_worker_active_write_soak`,
  `contract.tcp_server.test_tcp_server_stop_from_worker_callback_soak`,
  `contract.tcp_server.test_tcp_server_repeated_stop`,
  `contract.tcp_server.test_tcp_server_stop_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_send`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_state`,
  `contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed`,
  `contract.tcp_connection.test_tcp_connection_send_after_close`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_soak`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_write`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_shutdown`,
  `contract.tcp_connection.test_tcp_connection_repeated_shutdown`,
  `contract.tcp_connection.test_tcp_connection_cross_thread_force_close_pending_read`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_read_mixed_timing_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_soak`,
  `contract.tcp_connection.test_tcp_connection_force_close_pending_write_mixed_timing_soak`,
  TcpConnection lifecycle/read/write/cancel-close contracts, and
  `integration.tcp.test_tcp_server_client_echo` through
  `PostQueuedCompletionStatus`, `AcceptEx`, `ConnectEx`, `WSARecv`, and
  `WSASend`. The active Windows source selection points at the IOCP backend,
  the legacy select backend files have been removed from the active target, and
  a select-based Windows CI job must not be promoted as the performance target.
  The Windows install/package consumer gate also passes locally: the VS2026
  Debug build installs to `build-local-vs2026/_install`, and the external
  `tests/cmake/install_consumer` fixture configures, builds, and runs through
  `find_package(GameNetCore)` and `GameNet::core`. The Windows workflow uses
  the Visual Studio generator, Debug CTest, install, and external package
  consumer gates. The latest recorded green Windows job is `ci` #29, run
  `29079836593`, on release commit `c4818d4b3956c85830e04d4a1f32df4ad701d453`;
  all four Linux jobs in the same workflow also passed.
  The IOCP data-path design and implementation plan are recorded in
  `docs/superpowers/specs/2026-07-07-windows-iocp-data-path-design.md` and
  `docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`. See
  `docs/development/windows_iocp_milestone.md`.

## Phase 4 Implementation State

The Phase 4 entry evidence gate was satisfied by the annotated preview tag
`v0.1.0-core-preview`; no GitHub Release was published for that tag. Candidate
`a7fd77cbd2140041cebb3f900d5c609fafc2adad`
owns the repeat-soak and benchmark artifacts, while release commit
`c4818d4b3956c85830e04d4a1f32df4ad701d453` owns the final main-branch CI and
tag:

- [x] Fresh candidate-SHA remote CI evidence is recorded for Linux CMake, Linux ASan/UBSan, Linux TSan, Linux Release, and Windows MSVC IOCP.
- [x] The remote `long-soak` workflow has a green run recorded with run id, commit sha, repeat count, timeout, date, result, and duration.
- [x] `docs/migration_status.md`, `docs/development/ci.md`, and `docs/development/windows_iocp_milestone.md` have no pending evidence for the validated candidate.
- [x] TcpConnection, TcpClient/Connector, and TcpServer lifecycle/race tests have no known flaky entries.
- [x] Linux and Windows install/package consumers pass through `find_package(GameNetCore)` and `GameNet::core`.
- [x] Matching Release `gamenet.core_benchmark.v1` evidence is recorded for Linux
  epoll and Windows IOCP with commands, scenario parameters, backend, and
  completion mode.
- [x] PR #2 is merged and annotated tag `v0.1.0-core-preview` points at the
  validated release commit; this was a tag-only preview, not a GitHub Release.
- [x] PacketFramer has an approved active intent, independent target, invariants,
  contracts, segmented-input coverage, and deterministic round-trip smoke.
- [x] TransportEndpoint/TCP adapter, SessionManager, bounded LogicLoop queue,
  pipeline example/integration, and BroadcastRouter/Dispatcher are implemented
  behind independent targets and contracts.
- [x] Core remains free of reverse dependencies; the scope guard enforces the
  allowed component matrix for core and every installed Phase 4 layer.
- [x] The pre-hardening local baseline recorded Windows Debug/Release pass
  74/74 and a downstream consumer linking all six exported targets; this line
  is historical and does not describe the current test inventory.
- [x] The `final-v4` local hardening preflight frozen into candidate `5ebad2c1`
  passes 85/85 in Windows Debug,
  Windows Release, and Linux Clang Release, passes all 27 Python guards, and
  passes executable Linux/Windows Release package consumers against exact
  version `0.2.0`.
- [x] Full Windows MSVC ASan and Linux Clang ASan/UBSan pass 85/85 after the
  IOCP lifetime repairs; Linux TSan passes 61/61 after the TimerQueue fixture
  repair; real libFuzzer completes 1000 runs; and both local platform sets of
  fixed Release Phase 4 benchmarks report `status: ok` and validate.
- [x] The audited lifecycle and implementation blockers are closed locally:
  session
  transport identity and management-loop-only access, callback/timer execution
  admission, PacketFramer budgets and a real fuzz target, LogicLoop
  concurrency/lifecycle, and Pipeline authentication with physical
  IO/management/logic loop handoffs. Pipeline revocation now wins even when a
  callback retains state, synchronous Logic-stage stop cannot destroy an active
  handler, and deterministic tests cover one exact AUTH-plus-command batch and
  a SessionManager submit/drain overlap with two live producers.
- [x] Intent semantics resolve all 25 active targets and 74 explicit
  verification paths while preserving 16 documented frozen Core metadata
  exemptions. Configured-CMake dependency analysis proves the one-way
  production graph and rejects direct and transitive reverse edges.
- [x] Broadcast routing now enforces Router-only immutable plans, owner
  availability, exact rejection metrics/order, bounded fanout, and real
  multi-loop TCP integration contracts across eight reconnect cycles.
- [x] The repository CI definition contains six producer jobs, including Linux
  sanitizer/fuzz/TSan gates and both Windows Debug IOCP and Windows Release
  package execution gates, plus one aggregation-only evidence-set gate. The
  long-soak workflow writes structured exact-repeat manifests and the Phase 4
  benchmark workflow requires a paired Linux/epoll plus Windows/IOCP manifest.
- [x] Pre-hardening PR #4 head `0d62054e148a1c95793799eb88856363ac6843d3`
  has historical five-job evidence in successful `ci` run `29147391402` (#32).
  It is not current-candidate evidence after the remediation changes.
- [x] Commit and push functional candidate
  `5ebad2c1a4a9487437340935e21f7468140c7e8d`; validate it through six main-CI
  producers plus `gamenet.ci_evidence_set.v1` in run `29160903594`, exact
  repeat-50 evidence in `29161167423`, and the paired Phase 4 benchmark gate in
  `29161168417`.
- [x] Move PR #4 to Ready and merge under explicit owner authorization without
  replacing the verified candidate; verify the merge tree, pass exact-commit
  main CI `29168786199`, and publish annotated tag `v0.2.0-phase4-preview`,
  checksums, and the formal GitHub Preview Release. Independent submitted review
  remained absent and is recorded as a process limitation.
- [ ] HTTP, RPC, UDP/KCP, TLS, coroutine, and a formal all-in-one pipeline library
  remain deferred until separately promoted.

Before promoting a later Preview or stable release, keep the Linux and Windows
CI gates green and record any missing soak, race-oriented, or platform-specific
verification as its own immutable validation step.
