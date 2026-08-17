# Continuous Integration

The CI gate keeps the migration focused on the frozen Reactor/TCP foundation
plus the explicitly active Phase 4 foundations.

## Phase 4 Preview Evidence

Functional candidate `5ebad2c1a4a9487437340935e21f7468140c7e8d` is committed and
pushed, and was the Draft PR #4 head when candidate evidence was produced.
Pull-request `ci` run
`29160903594` checked GitHub merge-ref
`e461b597f2642e000717f536f3b430b804ba26ad` while binding both candidate and
PR-head identity to `5ebad2c1a4a9487437340935e21f7468140c7e8d`; the six producer
jobs and aggregation-only evidence gate passed 7/7. The same functional candidate
also owns successful manual `long-soak` run `29161167423`, which verified exactly
3,050/3,050 threading and 400/400 Pipeline/Broadcast executions, and successful
benchmark run `29161168417`, whose Linux/epoll and Windows/IOCP producers fed a
green paired evidence gate.

Final evidence-only PR head `4abd5960...` also passed six producers plus the
aggregate gate in run `29162961320`. PR #4 was subsequently owner-authorized and
merged as `7668d6b82a0d815ccd79f83c572bc0a36bcceea0`; its tree equals the final
PR-head tree. Main push run
[`29168786199`](https://github.com/YanqingXu/game-net-core/actions/runs/29168786199)
validated that exact release commit 7/7. Its aggregate artifact
`ci-evidence-set-7668d6b82a0d815ccd79f83c572bc0a36bcceea0-29168786199-1`
has ZIP SHA-256
`2bd08a1bcb1c502ef5b6a79cd3a6cd79f0e687e6ce3bc2faeddfdcecda0aa9e3`.
The annotated tag and
[formal Preview Release](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview)
are published. PR #4 recorded no submitted GitHub review; this remains a process
limitation rather than a CI claim.

## Scope

The `ci` workflow validates:

- CMake configure with C++23 and testing enabled.
- Build-governance guard for the Linux/Windows target-system allow-list,
  explicit static installed targets, rejected shared-library builds, and
  configure-time failure for unimplemented TLS/experimental options.
- Public API manifest v2 verification for classified exported targets and
  headers, plus a deterministic diff against the immutable 0.2 preview
  snapshot. Producer checkouts fetch full history so the snapshot inventory
  and stable fingerprints are recomputed from the declared release tag.
- Scope boundary guard for deferred modules and legacy `mini_trantor` symbols.
- Intent consistency guard for active module paths and support intents.
- Intent metadata contract guard for complete active/deferred/legacy catalogs,
  target/promotion gates, and stale source-project wording in active intents.
- Immutable migration-provenance guard for the active Phase 4 intent paths,
  checked against the pinned `YanqingXu/mini_trantor` Git object before the
  semantic guard reads the source checkout.
- MSVC `/utf-8` and `/FS` compile-option guard so Windows builds parse UTF-8
  source/header comments consistently and parallel Debug builds serialize PDB
  writes safely.
- Short generated CMake test target names so verbose contract test identifiers
  remain visible in CTest while MSBuild intermediate `.tlog` paths stay below
  Windows path-length limits.
- Windows IOCP milestone guard for
  `docs/development/windows_iocp_milestone.md`, keeping Windows support tied
  to the IOCP backend and the Windows MSVC workflow gate.
- Windows IOCP data-path guard for the overlapped operation layer, socket
  extension helpers, completion metadata translation, and atomic wakeup
  coalescing/reset.
- EventLoop contract guard for owner-thread scheduling, quit, bounded future
  wait helper coverage, and deterministic wakeup reset-race coverage.
- Logger thread-contract guard for concurrent emission, runtime callback
  replacement, callback snapshot/re-entry semantics, and TSan selection.
- EventLoopThreadPool contract guard for queued-work race/soak coverage.
- TimerQueue contract guard for ready-timer cancellation races and explicit
  fixed-delay/fixed-rate bounded catch-up semantics.
- EventLoopThreadPool contract guard for round-robin, exact least-connection
  accounting, pending-functor queue-lag selection, and stable rendezvous
  consistent hashing.
- Threading gate contract guard for the race-oriented TSan test selection.
- TcpConnection thread-contract guard for atomic public state observation and
  owner-loop-only callback/socket-option mutation.
- Core benchmark contract guard for the default-off, non-CTest target,
  versioned JSON schema, three scenarios, platform resource sampling, and docs.
- Phase 4 benchmark contract guard for framing throughput, logic queue-lag/P99,
  broadcast fanout latency/process working-set high water, and ownership markers.
- Long-soak workflow guard for the non-default repeated threading contract gate.
- Manual core-benchmark workflow guards for paired Core and Phase 4
  Linux/Windows Release raw JSON artifacts, same-runner baseline/candidate
  median regression budgets, and no push/PR triggers.
- Sanitizer CMake contract check so ASan/UBSan and TSan flags apply to the
  core target itself as well as dependent tests/examples.
- Core and active Phase 4 target builds.
- Intent target/test semantic guard based on executed CMake configure trace and
  configured CTest JSON inventory, with isolated negative fixtures for phantom
  targets/tests, artifact-kind drift, and missing Phase 4 migration provenance.
- Echo and game-server-pipeline examples build.
- Unit, contract, and integration tests through CTest.
- Install and `find_package(GameNetCore)` consumer configure, build, and
  executable-run verification on Linux and Windows.
- ASan/UBSan Debug build and CTest suite on Linux.
- TSan Debug build and `threading`-labeled CTest suite on Linux.
- Release build and CTest suite on Linux.
- Windows MSVC Debug build, CTest suite, and install/package consumer
  verification through the IOCP backend.
- Windows MSVC Release build and CTest suite as a required main-CI job.

It keeps post-Phase-4/experimental modules disabled:

- TLS
- experimental transport
- HTTP, WebSocket, RPC, UDP, KCP, PMTU/FEC, metrics, coroutine, and a formal
  all-in-one game pipeline library

Those deferred intents remain design assets until separately promoted. Active
Phase 4 targets are protocol, transport, game_session, game_logic, and broadcast;
the pipeline remains non-installed example support.

The scope guard runs before CMake configure:

```bash
python3 tests/scope/test_scope_guard.py
python3 tests/scope/test_intent_consistency.py
python3 tests/scope/test_intent_metadata.py
python3 tools/verify_migration_provenance.py
python3 tests/scope/test_intent_semantics.py
python3 tools/check_scope_boundaries.py
python3 tests/cmake/test_sanitizer_flags.py
python3 tests/cmake/test_install_package_contract.py
python3 tests/cmake/test_packet_framer_fuzz_contract.py
python3 tests/cmake/test_core_benchmark_contract.py
python3 tests/cmake/test_phase4_benchmark_contract.py
python3 tests/cmake/test_capacity_profile_contract.py
python3 tests/cmake/test_logger_thread_contract.py
python3 tests/cmake/test_event_loop_contracts.py
python3 tests/cmake/test_event_loop_thread_pool_contracts.py
python3 tests/cmake/test_migration_status_contract.py
python3 tests/cmake/test_msvc_utf8_contract.py
python3 tests/cmake/test_platform_backend_contract.py
python3 tests/cmake/test_build_governance_contract.py
python3 tests/cmake/test_tcp_lifecycle_contracts.py
python3 tests/cmake/test_tcp_connection_context_contract.py
python3 tests/cmake/test_tcp_connection_thread_contract.py
python3 tests/cmake/test_timer_queue_contracts.py
python3 tests/cmake/test_threading_gate_contracts.py
python3 tests/cmake/test_windows_iocp_milestone_contract.py
python3 tests/cmake/test_windows_iocp_data_path_contract.py
python3 tests/cmake/test_release_safe_tests.py
python3 tests/ci/test_workflow_jobs.py
python3 tests/ci/test_long_soak_workflow.py
python3 tests/ci/test_core_benchmark_workflow.py
python3 tests/ci/test_phase4_benchmark_workflow.py
```

The semantic guard depends on the migration source, so every CI and long-soak
job that runs it first checks out `YanqingXu/mini_trantor` into
`mini_trantor/` at commit
`3eba368475a68f677aae920d4f299b155db23d57`, with persisted credentials
disabled. `tools/verify_migration_provenance.py` verifies that exact checkout
HEAD and resolves every active Phase 4 `source_paths` entry as a file in the
pinned Git object. An ignored or otherwise untracked working-tree directory is
therefore not accepted as provenance by itself.

For an equivalent local guard run, create the source checkout once and detach
it at the same commit before running the guard list above:

```bash
git clone https://github.com/YanqingXu/mini_trantor.git mini_trantor
git -C mini_trantor checkout --detach 3eba368475a68f677aae920d4f299b155db23d57
python3 tools/verify_migration_provenance.py
```

## Main CI Evidence Contract

All six required jobs have bounded job runtimes: Linux Debug and Release use
30 minutes, while ASan/UBSan, TSan, Windows Debug, and Windows Release use 45
minutes. Immediately after configure, each job runs
`tools/verify_ctest_inventory.py`, requires exactly 120 configured tests, and
writes `ci-evidence/ctest-inventory.json`. The TSan job additionally requires
`threading=93` before it builds and executes that labeled slice. Inventory drift
therefore fails before compilation or test selection can create a false-green
result.

Every main CTest invocation writes `ci-evidence/ctest-junit.xml` with
`--output-junit` and `ci-evidence/ctest.log` with `--output-log`. Linux jobs use a 60-second per-test timeout; Windows main
suites use a 30-second lifecycle timeout. Linux Debug and both Windows
jobs require exactly 2 configured install-consumer tests before building them,
then write `install-consumer-inventory.json`, `install-consumer-junit.xml`, and
`install-consumer-ctest.log` with a 60-second timeout for the installed-package
executables. JUnit records suite and test durations. Total job
elapsed time remains authoritative in GitHub Actions job metadata because a
step running before artifact upload cannot reliably know the final upload
duration. CTest 3.21 or newer is required for evidence capture through
`--output-junit`; this does not change the project's CMake 3.20 configure/build
minimum for developers who are not producing the CI evidence bundle.

Each job records `toolchain.txt`. The Windows files include the Visual Studio
installation and default MSVC toolset version; the sanitizer job also records
Clang. The ASan/UBSan libFuzzer smoke copies the seed corpus and dictionary into
`build-fuzz/`, then copies the running corpus and dictionary into
`ci-evidence/fuzz/`. It uses shell `pipefail`, a fixed `-seed=424242`,
`-print_final_stats=1`, saves `fuzz/fuzzer.log`, and directs reproducer/crash
files through `-artifact_prefix=ci-evidence/fuzz/artifacts/`. The smoke uses
exactly `-runs=1000` with no competing total-time stopping limit, while the
per-input timeout remains 2 seconds. The aggregate verifier parses both the
`#1000 DONE` marker and `stat::number_of_executed_units: 1000`; a shorter
normal exit, missing final statistics, or merely present log is rejected.

After the final validation step, `tools/write_ci_evidence.py` runs under
`if: always()` and writes schema `gamenet.ci_evidence.v1`. The manifest records
the candidate SHA, actual `checkout_sha`, GitHub SHA, optional PR-head SHA,
merge/current SHA, run id, attempt, job, workflow, ref, UTC date, runner OS and
architecture, normalized commands, `pre_upload_status`, and
the byte count and SHA-256 of every other file that actually exists under
`ci-evidence/` (the manifest cannot hash itself). Its `artifact_name` exactly
matches the upload name. The status
scope is `job_before_evidence_upload`: an upload failure may still change the
final GitHub job result and is not retroactively represented as a successful
upload.

The writer rejects identity drift instead of merely recording it. The actual
`checkout_sha` must equal both `GITHUB_SHA` and the merge/current SHA. For a
`pull_request` event, the candidate SHA must equal the PR-head SHA; for every
other event, the candidate SHA must equal `GITHUB_SHA`. Main CI also passes
`--require-canonical-artifact-name`, which requires the artifact name to end in
the ordered, hyphen-delimited job/GitHub-SHA/run-id/run-attempt identity. These
relations have executable positive and negative fixtures in
`tests/ci/test_workflow_jobs.py`.

The following `if: always()` upload uses a unique SHA/run/attempt-bound name:

```text
ci-evidence-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
```

It uploads the complete `ci-evidence/` directory and, under the default
`required` artifact policy, treats an absent evidence root or upload failure as
an error. Inventory, JUnit, raw CTest logs, toolchain, manifest,
install-consumer evidence where applicable, and ASan fuzz inputs/logs/artifacts
are retained for 90 days when the artifact service accepts the upload. The
primary Linux producer also retains
`public-api-diff.json`, whose target/header additions, removals, category moves,
stable fingerprint changes, and compatibility-decision signal are hashed by
that producer manifest. An early failure may legitimately leave later files
absent; the manifest hashes only the evidence that was actually produced rather
than fabricating a passing result.

A seventh, aggregation-only job depends on all six producer jobs and runs even
when one producer failed. It first requires all six producer job results to be
`success`, independently of artifact availability. Under the default
`required` policy it then downloads exactly the current run/attempt producer
artifacts and executes `tools/verify_ci_evidence_set.py`. The verifier requires
the exact six job names and one common checkout/candidate/PR-head/merge/current
SHA plus run/attempt identity, requires every producer and pre-upload status to
be `success`, and recomputes every byte count and SHA-256 declared by each
producer manifest. It parses the inventory and JUnit rather than trusting file
presence: five jobs must execute 120 tests, TSan must execute the exact 93-test
`threading` selection from the 120-test inventory, and Linux Debug plus both
Windows jobs must each execute exactly one installed-package consumer test.
The resulting `gamenet.ci_evidence_set.v1` manifest includes each producer
manifest hash and is uploaded for 90 days as:

```text
ci-evidence-set-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
```

Therefore a green producer subset, mismatched rerun, renamed artifact, corrupt
file, inventory-only count, or JUnit selection drift cannot satisfy the main CI
evidence gate.

It fails the workflow if active intents use legacy `mini/net` paths, if source
uses `mini/` includes or `mini::` namespaces, if inactive component paths or
targets appear, or if an installed layer references a component outside the
allowed one-way dependency matrix. Promoting a component must update active
scope, intent metadata, dependency rules, tests, and migration status together.

## Remote Evidence Boundary

Record immutable validation evidence instead of a self-referential current HEAD:

The current Phase 4 candidate evidence is recorded at the start of this
document. The following entries preserve the completed Phase 3.5 Core Preview
chain and earlier candidate history:

- Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`
  (the fixed label belongs to the Phase 3.5 Core release record; current Phase
  4 candidate evidence is recorded above).
- CI workflow run id: `29079836593` (`ci` #29, `main`).
- Validation date: 2026-07-10.
- Result: Linux Debug, ASan/UBSan, TSan, Release, and Windows MSVC IOCP passed.
- Release tag: `v0.1.0-core-preview`, annotated and peeled to the validated commit.

The focused code candidate
`a7fd77cbd2140041cebb3f900d5c609fafc2adad` was first validated by PR `ci`
run `29076601085` (#27), then preserved as a parent of the release merge commit.
The same candidate owns the repeat-50 and cross-platform benchmark evidence
recorded below; the merge commit only adds its evidence-documentation parent.

The preceding audited candidate,
`d1474b5f32e609a7d2e2648af31b45635595d304`, failed in run id `29073362905`
(`ci` #26) on
`contract.tcp_client.test_tcp_client_repeated_connect` at
`serverConnectedCount == 1`. On Linux, the first connection can close
synchronously inside its connection callback; duplicate cross-thread
`connect()` submissions that were already queued then observed no active
TcpConnection and restarted Connector. The validated fix admits one
generation-tagged connect request per pending/active lifecycle and releases
that admission after terminal no-retry failure or connection removal. Local
Debug and Release pass 67/67 tests, and the repaired contract passes 50 repeats.
At that point, no later commit could be described as validated without a new
run id, commit, date, and complete job result. Phase 4 candidate `5ebad2c1`
now satisfies that requirement through the current evidence chain above.

The ASan/UBSan job uses:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_ASAN_UBSAN=ON
python3 tools/verify_ctest_inventory.py --test-dir build-asan --expected-total 120 --output ci-evidence/ctest-inventory.json
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure --timeout 60 --output-junit "$PWD/ci-evidence/ctest-junit.xml" --output-log "$PWD/ci-evidence/ctest.log"
```

The TSan job is race-oriented rather than a replacement for full Debug CTest.
It builds the same core/test targets with ThreadSanitizer enabled and runs the
tests labeled `threading`. That label intentionally covers cross-thread APIs,
worker-loop scheduling, and lifecycle tests with cancel/close race risk such as
pending-read and pending-write `forceClose()` contracts,
direct `Connector::stop()` retry-timer cancellation contracts,
mixed-timing pending-ConnectEx `TcpClient::stop()` contracts, and
active retry-enabled `TcpClient::stop()` after peer-close contracts,
active cross-thread `TcpClient::disconnect()` contracts,
repeated active `TcpClient::disconnect()` idempotence contracts,
repeated active `TcpClient::stop()` idempotence contracts,
repeated active `TcpClient::connect()` idempotence contracts,
active cross-thread `TcpClient::connect()` contracts,
active cross-thread `TcpClient` retry configuration contracts,
concurrent Logger emission/configuration contracts,
cross-thread `TcpConnection` state observation contracts,
post-close `TcpConnection::send()` ignore contracts,
mixed-timing pending-read `TcpConnection::forceClose()` contracts,
mixed-timing pending-write `TcpConnection::forceClose()` contracts,
repeated `TcpConnection::shutdown()` idempotence contracts, plus
worker-owned active-write `TcpServer::stop()` contracts, worker-callback
`TcpServer::stop()` contracts, and repeated `TcpServer::stop()` idempotence contracts:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TSAN=ON
python3 tools/verify_ctest_inventory.py --test-dir build-tsan --expected-total 120 --expect-label threading=93 --output ci-evidence/ctest-inventory.json
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure -L threading --timeout 60 --output-junit "$PWD/ci-evidence/ctest-junit.xml" --output-log "$PWD/ci-evidence/ctest.log"
```

The Linux Release job uses:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DGAMENET_BUILD_TESTING=ON
python3 tools/verify_ctest_inventory.py --test-dir build-release --expected-total 120 --output ci-evidence/ctest-inventory.json
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure --timeout 60 --output-junit "$PWD/ci-evidence/ctest-junit.xml" --output-log "$PWD/ci-evidence/ctest.log"
```

The Windows Release job is separate from the Windows Debug/installation job so
both configurations remain explicit required checks:

```powershell
cmake -S . -B build-windows-release -G "Visual Studio 18 2026" -A x64 -DGAMENET_BUILD_TESTING=ON
python tools/verify_ctest_inventory.py --test-dir build-windows-release --config Release --expected-total 120 --output ci-evidence/ctest-inventory.json
cmake --build build-windows-release --config Release --parallel
ctest --test-dir build-windows-release -C Release --output-on-failure --timeout 30 --output-junit "$pwd/ci-evidence/ctest-junit.xml" --output-log "$pwd/ci-evidence/ctest.log"
cmake --install build-windows-release --config Release --prefix "$pwd/build-windows-release/_install"
cmake -S tests/cmake/install_consumer -B build-windows-release-install-consumer -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$pwd/build-windows-release/_install"
python tools/verify_ctest_inventory.py --test-dir build-windows-release-install-consumer --config Release --expected-total 2 --output ci-evidence/install-consumer-inventory.json
cmake --build build-windows-release-install-consumer --config Release --parallel
ctest --test-dir build-windows-release-install-consumer -C Release --output-on-failure --timeout 60 --output-junit "$pwd/ci-evidence/install-consumer-junit.xml" --output-log "$pwd/ci-evidence/install-consumer-ctest.log"
```

A local Windows MSVC AddressSanitizer checkpoint uses the same inventory. The
test registration prepends the selected MSVC compiler directory to each test's
`PATH`, so `clang_rt.asan_dynamic-x86_64.dll` is found without a developer-shell
side effect:

```powershell
cmake -S . -B build-asan-windows -G "Visual Studio 18 2026" -A x64 -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_ASAN_UBSAN=ON
python tools/verify_ctest_inventory.py --test-dir build-asan-windows --config Debug --expected-total 120 --expect-label threading=93
cmake --build build-asan-windows --config Debug --parallel
ctest --test-dir build-asan-windows -C Debug --output-on-failure --timeout 60
```

The historical core-preview run predates this Windows Release main-CI job.
Phase 4 candidate run `29160903594` now validates the expanded six-producer plus
aggregate gate; the older Core run remains historical evidence only.

The C++ tests use the `tests/support/TestAssert.h` helper instead of standard
`assert`, so contract checks remain active when Release builds define `NDEBUG`.
Debug, ASan/UBSan, and Release jobs are all test-execution gates.

### Manual Windows Self-Hosted IOCP

`.github/workflows/windows-self-hosted-ci.yml` is a manual-only fallback for a
trusted `main` commit when GitHub-hosted Windows capacity is unavailable. It
has only a `workflow_dispatch` trigger, rejects refs other than
`refs/heads/main`, uses read-only repository permissions, and targets the
dedicated labels `[self-hosted, windows, x64, gamenet-windows]`. Its matrix runs
Debug and Release serially on the single runner, verifies the complete
120-test inventory, executes the repository guards, installs the package,
runs the external consumer, and uses deliberately short `b` and `c` build
directories to remain below the legacy Windows `MAX_PATH` limit.

Each configuration writes its evidence manifest to the job log and job summary.
The workflow also attempts to retain the full configuration-specific evidence
artifact for 90 days, but that upload is non-blocking because repository or
account storage quota exhaustion must not override the build and test result.

This fallback does not run for `pull_request` or `push`. The repository is
public, so the Windows runner must not be exposed to unreviewed PR-controlled
workflow definitions. The manual Windows evidence complements local and
GitHub-hosted results; it does not replace Linux, ASan/UBSan, TSan, repeat, or
the six-producer aggregate gate.

### Billing-Safe Main CI Routing

Main `ci` defaults to GitHub-hosted runners and strict artifact retention when
the repository variables are unset. During an account billing lock that blocks
GitHub-hosted scheduling, trusted maintainers may temporarily set:

```bash
gh variable set GAMENET_CI_RUNNER_MODE --body self-hosted
gh variable set GAMENET_CI_ARTIFACT_POLICY --body best-effort
```

`GAMENET_CI_RUNNER_MODE=self-hosted` routes the four Linux producers and the
aggregate job to `[self-hosted, linux, x64, gamenet-endurance]`, and the two
Windows producers to `[self-hosted, windows, x64, gamenet-windows]`. This route
is allowed only for `push`, manual dispatch, and a trusted same-repository PR.
Fork PRs continue to select GitHub-hosted runners so an untrusted workflow
cannot execute on repository-owned machines. Both dedicated runners must be
online; the workflow never skips a required platform job.

`GAMENET_CI_ARTIFACT_POLICY=best-effort` changes only artifact upload/download
handling. Repository guards, configure, inventory, build, CTest, sanitizer,
package-consumer, and all six producer job-result checks remain mandatory. An
artifact quota or storage failure is recorded as a warning and raw step
outcome. When all six complete producer artifacts are unavailable, aggregation
is intentionally omitted: the run provides no aggregate evidence and must not
be cited as a `gamenet.ci_evidence_set.v1` or release-evidence result.

Restore the normal gate after the account lock is cleared:

```bash
gh variable delete GAMENET_CI_RUNNER_MODE
gh variable delete GAMENET_CI_ARTIFACT_POLICY
```

Deleting the variables restores `ubuntu-24.04`, `windows-latest`, and the
`required` artifact policy.

## Non-Default Long Soak

The `long-soak` workflow is manual-only through `workflow_dispatch`; it is not
attached to `push` or `pull_request`. It complements the regular CI and TSan
jobs by rebuilding the current targets and running two repeat-until-failure
gates on the dedicated Linux/epoll self-hosted runner selected by
`[self-hosted, linux, x64, gamenet-endurance]`. Its current dispatch input
defaults to repeat 50. The 60-second per-test timeout applies to both gates:

- the complete inventory of 93 threading-labeled tests;
- the focused inventory of 12 Pipeline/Broadcast tests, repeated separately so
  Phase 4 lifecycle, handoff, ordering, and fanout failures are visible as a
  dedicated result.

The same workflow's `ci` mode runs a serial four-profile matrix on that
self-hosted runner: Debug and Release execute all 120 tests plus the installed
package consumer, ASan/UBSan executes all 120 tests, and TSan executes the exact
93-test threading inventory. Every profile verifies the immutable checkout SHA,
public API diff and repository guards, then writes and uploads its own
attempt-bound `gamenet.ci_evidence.v1` artifact. Serial execution prevents the
four sanitizer/build profiles from competing for one runner's CPU and memory;
each self-hosted build also uses `--parallel 1` so compiler fan-out cannot
exhaust the endurance host.

The `ci_artifact_policy` dispatch input applies only to that four-profile
self-hosted `ci` matrix. It defaults to `best-effort`: artifact upload is still
attempted, but an unavailable GitHub artifact service, storage quota, or account
billing lock encountered after the self-hosted job has started is reported as a
workflow warning and does not replace a successful build/test result with an
upload failure. Selecting `required` restores the strict upload gate. The job
summary always records the selected policy and the raw upload outcome. An
account-level lock that prevents GitHub from scheduling the workflow at all
cannot be bypassed by repository YAML.

Best-effort success proves only the logged repository guards, build, CTest, and
package-consumer result. If upload failed, it does not provide retained artifact
evidence and must not be cited as satisfying a formal artifact or release
evidence gate. Repeat-soak and 24/72-hour production-endurance uploads remain
strict and are not affected by this input.

LeakSanitizer needs to attach to the sanitizer process while collecting thread
roots. The dedicated trusted runner must therefore persist Yama
`kernel.yama.ptrace_scope=0`; merely observing `TracerPid: 0` is insufficient.
Configure and verify the host once:

```bash
printf '%s\n' 'kernel.yama.ptrace_scope=0' |
  sudo tee /etc/sysctl.d/99-gamenet-lsan.conf
sudo sysctl -p /etc/sysctl.d/99-gamenet-lsan.conf
test "$(cat /proc/sys/kernel/yama/ptrace_scope)" = 0
```

This weakens same-UID ptrace isolation for the host, so it belongs only on the
dedicated trusted endurance runner and must not be copied to a shared or
untrusted-execution machine. The ASan/UBSan profile records `TracerPid` and
`ptrace_scope` in `ci-evidence/asan-ubsan-runner.txt` and fails before repository
guards or compilation when the shell is traced or the numeric Yama scope is not
zero. Leak detection remains enabled; the gate must not be made green with
`ASAN_OPTIONS=detect_leaks=0`.

After configure, `tools/verify_ctest_inventory.py` records the complete 120-test
JSON inventory and requires `threading=93`, `game_pipeline=7`, and
`broadcast=5` before either repeat begins. Adding or relabeling a test must
therefore update the workflow contract and this evidence ledger intentionally.

Long-soak repository guards include `tests/cmake/test_event_loop_contracts.py`
so the manual soak guard surface stays aligned with the ordinary `ci` workflow
before CMake configure.

After both CTest invocations, `tools/verify_ctest_repeat_evidence.py` parses the
raw result lines against that inventory. It requires every selected test to
pass the exact dispatch repeat count, rejects missing/extra/non-passing result
lines, verifies the exact final CTest success summary, and writes separate
`gamenet.ctest_repeat_evidence.v1` summaries. At the default, they prove
82 x 50 = 4,100 threading executions and 12 x 50 = 600 focused executions, while
recording command, timeout, real CTest time, per-test counts, and inventory/log
SHA-256 hashes.

The 90-day artifact name is canonical and attempt-bound:

```text
long-soak-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
```

It contains the shared `gamenet.ci_evidence.v1` manifest, JSON inventory, both
structured repeat summaries, raw repeat logs, and toolchain. The writer enforces
checkout/GitHub/current SHA equality and the artifact identity suffix, so a soak
result cannot be detached from its tested commit, run, or rerun attempt. The
manifest is written only after both structured repeat verifiers and records the
live `job.status`; it never hard-codes success. An earlier failure can therefore
produce partial forensic evidence with a failure manifest, but cannot produce a
successful manifest that predates either repeat summary.

```bash
cmake -S . -B build-long-soak -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TLS=OFF -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-long-soak --parallel 1
python3 tools/verify_ctest_inventory.py --test-dir build-long-soak --expected-total 120 --expect-label threading=93 --expect-label game_pipeline=7 --expect-label broadcast=5 --output long-soak-evidence/ctest-inventory.json
ctest --test-dir build-long-soak --output-on-failure -L threading --repeat until-fail:<repeat> --timeout <timeout_seconds>
ctest --test-dir build-long-soak --output-on-failure -L "game_pipeline|broadcast" --repeat until-fail:<repeat> --timeout <timeout_seconds>
python3 tools/verify_ctest_repeat_evidence.py --inventory long-soak-evidence/ctest-inventory.json --log long-soak-evidence/threading-repeat.log --selection-label threading --expected-selected 93 --repeat <repeat> --timeout-seconds <timeout_seconds> --command <exact-command> --output long-soak-evidence/threading-repeat-evidence.json
```

Use it for phase hardening evidence when mixed-timing lifecycle contracts need
more iteration than the ordinary PR gate should spend.

The same manual workflow also exposes fixed `candidate-24h` and `release-72h`
modes on that dedicated Linux/epoll self-hosted runner. These modes run one
uninterrupted fault-injection process, retain monotonic heartbeat/checkpoint
evidence, record the exact workflow run/attempt, and reject shortened duration
overrides. Both production modes require an exact capacity run/attempt and
revalidate its paired raw evidence before the long process starts:
`candidate-24h` consumes `candidate-10k`, while `release-72h` consumes
`dedicated-100k`. The 72-hour mode additionally downloads the exact successful
24-hour run/attempt from the same candidate SHA.

After the uninterrupted process passes,
`tools/verify_production_promotion_evidence.py` revalidates all retained
capacity samples, pair manifest, endurance results, and logs. It emits
`gamenet.production_promotion_evidence.v1`, binding 10k + 24h for candidate
promotion or dedicated 100k + 24/72h for release promotion. The outer
`gamenet.ci_evidence.v1` manifest hashes that promotion result with the
remaining job evidence. See
`docs/development/production_endurance.md`.

Phase 3.5 historical evidence: run `29077148022`, job `86311227712`, commit
`a7fd77cbd2140041cebb3f900d5c609fafc2adad`, repeat 50, timeout 60 seconds,
completed successfully at 2026-07-10T08:04:12Z. CTest reported 46/46
threading-labeled tests passed and total real test time of 1632.47 seconds;
the complete workflow job took 28m27s. The command recorded in the log was:

```bash
ctest --test-dir build-long-soak --output-on-failure -L threading --repeat until-fail:50 --timeout 60
```

That 46-test run does not validate the current 64-test threading inventory or
the separate 8-test Pipeline/Broadcast repeat. Candidate SHA-bound `long-soak`
run `29161167423` now supplies that evidence: 3,050/3,050 and 400/400 exact
executions, followed by successful structured verification, manifest creation,
and artifact upload.

Earlier evidence remains useful as history but no longer defines the gate:
run `28986707243`, job `86017363504`, commit
`9b27a0a3c3993cb1f90ef4357fa80027205ca221`, repeat 20, timeout 60 seconds,
completed successfully at 2026-07-09T01:15:38Z with 36/36 tests passed in
608.67 seconds.

Local Windows Debug evidence before the cross-thread
TcpClient retry configuration contract was added: the same repeat shape
with `ctest --test-dir build -C Debug --output-on-failure -L threading --repeat until-fail:20 --timeout 60`
passed the previous 43-test threading slice on 2026-07-09; CTest reported
43/43 threading-labeled tests passed across 20 repeats and total test time was
637.56 seconds. The then-expanded 44-test threading slice was covered once
by the full Windows Debug and Release CTest checkpoint; repeat-soak evidence
for that slice is not recorded here. The cross-thread TcpConnection state
contract added afterward expanded the threading slice to 45 tests. The Logger
thread-safety contract expanded the then-present threading slice to 46 tests.

Historical local Windows preflight on 2026-07-10 passed 67/67 Debug tests in
43.57 seconds and 67/67 Release tests in 37.38 seconds. The repaired
`contract.tcp_client.test_tcp_client_repeated_connect` passes 50 repeats in
32.41 seconds with a 5-second per-test timeout. The full then-current worktree
46-test threading
slice across 5 repeats passes in 176.90 seconds with a 15-second per-test timeout.
The Release install plus external `find_package(GameNetCore)` / `GameNet::core`
consumer also configures, builds, and exits successfully. These local results
belong to the earlier Core inventory and no longer define the Phase 4 gate.

The final local Windows Debug preflight before candidate freeze uses the current
85-test inventory. The `threading=61` selection passed repeat 50 for exactly
3,050 executions in 1,777.76 seconds, and the separate
`game_pipeline|broadcast` selection passed 8 x 50 = 400 executions in 54.16
seconds. Both raw logs passed `tools/verify_ctest_repeat_evidence.py`; their
`gamenet.ctest_repeat_evidence.v1` summaries share inventory SHA-256
`37ee7fb3572c911fa771ba42ce1fcb91a252bc2c78c56b98b280f5305c77a09a`.
These local results are supporting preflight; remote run `29161167423` is the
authoritative exact-repeat evidence for candidate `5ebad2c1`.

## Remote Core Benchmark Evidence

The current Phase 6 workflow additionally builds fixed baseline
`2b1be4343f7c478eb40542451f30aad8ca474003` beside the candidate on each runner,
runs 12 Core/Phase-4 scale scenarios three times per revision, and writes
`gamenet.performance_regression.v1`. The paired verifier requires both
platforms to pass independently against the same reviewed budget and rehashes
all matrix samples. Raw Linux and Windows scores are never compared directly.
See `docs/development/performance_regression.md`.

Manual `core-benchmark` run `29077151229` completed successfully on commit
`a7fd77cbd2140041cebb3f900d5c609fafc2adad` at 2026-07-10T07:37:15Z. It
published two SHA-bound Release artifacts:

- `core-benchmark-linux-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad`
- `core-benchmark-windows-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad`

Those are historical names from that run. The current workflow makes reruns
collision-free by binding every Core raw artifact to job/SHA/run/attempt:

```text
core-benchmark-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
```

Each artifact contains `echo-1-worker.json`, `echo-2-workers.json`,
`connections-256.json`, and `slow-client-4.json`. All eight files use schema
`gamenet.core_benchmark.v1` and report `status: ok`. Echo uses four connections,
10,000 messages per connection, and 256-byte payloads. The connection scenario
uses 256 idle connections. The slow-client scenario offers 8 MiB to each of
four non-reading clients with a 65,536-byte high-water mark.

Linux identifies backend `epoll` and completion mode `epoll_wait_batch`; its
one/two-worker echo snapshots are 32.337/65.234 MiB/s with P99 97.419/74.229 us,
the connection working-set delta is 991,232 bytes (3,872 bytes per connection),
and slow-client working-set delta is 26,562,560 bytes with four high-water
callbacks. Historical Windows Core Preview evidence identifies backend `iocp`
and completion mode `single_get_queued_completion_status`; current M3
candidates report `get_queued_completion_status_ex_batch_64`. Its
one/two-worker snapshots are
16.188/26.110 MiB/s with P99 162.8/151.3 us, the connection working-set delta
is 18,137,088 bytes (70,848 bytes per connection), and slow-client working-set
delta is 67,493,888 bytes with four high-water callbacks. These are raw
environment-specific snapshots, not performance thresholds.

## Mixed Capacity Gate

The manual-only `.github/workflows/capacity-gate.yml` workflow produces paired
Linux/epoll and Windows/IOCP evidence for the scale-ready mixed
slow-reader/healthy-probe profile. `candidate-10k` runs three repetitions on
hosted runners; `dedicated-100k` runs one repetition only on the provisioned
capacity runner labels and requires the exact `RUN_DEDICATED_100K`
acknowledgement.

Each producer invokes `tools/run_capacity_gate.py`, whose fixed profile owns
the connection scale, Broadcast endpoint-attempt scale, healthy-probe scale,
watermarks, timeout, and bounded recovery-reader count. Every raw
`gamenet.capacity_profile.v3` sample is strictly validated before the producer
writes a hashed `gamenet.capacity_gate.v1` manifest.

The aggregation job first requires both producer jobs to finish successfully.
It downloads only the current attempt's canonical artifacts and runs
`tools/verify_capacity_gate_evidence_set.py`, which revalidates raw samples,
hashes, platform/backend identity, Release build identity, reviewed parameters,
and cross-platform profile equality before emitting
`gamenet.capacity_gate_pair.v1`. Raw timing, rate, and RSS measurements from
different hosts are not compared.

This workflow is an evidence producer, not an automatic release action. The
10k pair is one candidate-promotion input; the 100k pair is a separate
dedicated-host gate. Neither replaces the 24/72-hour one-process endurance
workflow.

## Phase 4 Benchmark Evidence Boundary

The same manual `core-benchmark` workflow now also builds
`gamenet_phase4_benchmark`. Its Linux/epoll and Windows/IOCP producer jobs each
record `framing.json`, `logic-queue.json`, `broadcast-fanout.json`, toolchain,
the semantic validation manifest, and a shared `gamenet.ci_evidence.v1` job
manifest. Producer artifact names are attempt-bound and canonical:

```text
phase4-benchmark-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
```

A third, aggregation-only `phase4-benchmark-evidence` job runs under
`if: always()`, then checks the Linux and Windows `needs.*.result` values in its
first step. Unless both producer final results are `success`, it fails before
downloading evidence. This closes the interval between writing the per-job
manifest and the producer's later Core/Phase 4 artifact uploads: a final upload
failure cannot be represented by an earlier successful manifest. After the
final-result check, the job downloads exactly the two producer artifacts from
the same SHA/run/attempt and runs
`tools/verify_phase4_benchmark_evidence_set.py`. It rechecks producer identity,
file bytes/hashes, toolchain/backend, exact scenario order, and identical input
parameters across platforms. Success emits
`gamenet.phase4_benchmark_pair_evidence.v1` in:

```text
phase4-benchmark-pair-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
```

The aggregate is the cross-platform evidence gate; it is not a third benchmark
producer and does not compare throughput values across different hosts.

Candidate benchmark run `29161168417` completed successfully for
`5ebad2c1a4a9487437340935e21f7468140c7e8d`. Both canonical Linux/epoll and
Windows/IOCP producer jobs passed, and the aggregation-only job verified and
uploaded `gamenet.phase4_benchmark_pair_evidence.v1`. This closes the functional
candidate's cross-platform benchmark evidence gate without treating throughput
values from different hosts as directly comparable.

## Install Package Gate

The Linux Debug job verifies that the split core target is consumable from
outside the source tree:

```bash
cmake --install build --prefix "$PWD/build/_install"
cmake -S tests/cmake/install_consumer -B build-install-consumer \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$PWD/build/_install"
python3 tools/verify_ctest_inventory.py --test-dir build-install-consumer --expected-total 2 --output ci-evidence/install-consumer-inventory.json
cmake --build build-install-consumer --parallel
ctest --test-dir build-install-consumer --output-on-failure --timeout 60 --output-junit "$PWD/ci-evidence/install-consumer-junit.xml" --output-log "$PWD/ci-evidence/install-consumer-ctest.log"
```

The consumer fixture uses `find_package(GameNetCore REQUIRED)` and registers
two CTest executables. The stable consumer links `GameNet::core`; the
provisional consumer links `protocol`, `transport`, `game_session`,
`game_logic`, and `broadcast`. Configure/build alone is not accepted as runtime
package evidence.

The Windows MSVC Debug job runs the same package consumer gate after installing
from `build-windows`, using the Visual Studio generator and `x64` platform. It
executes the consumer with `ctest -C Debug`. The Windows Release job repeats
install, external configure/build, and executable CTest with `-C Release`, so
both installed configurations are verified against the IOCP build at runtime
as well as link time.

## Current Platform Gate

The active CI gate defaults to `ubuntu-24.04` and `windows-latest`. The
temporary billing-safe route documented above preserves the same six producer
contracts on dedicated Linux and Windows self-hosted runners; it changes
scheduling and artifact retention policy, not the platform requirements.

Linux/epoll is the Tier 1 release, sanitizer, performance, and endurance
reference. Windows/IOCP remains a required Tier 2 functional and package
platform until the M3 batching, synchronous-error, ownership, wakeup, and
capacity promotion gates complete. CMake rejects macOS, BSD variants, other
target systems, shared-library requests, and enabled unimplemented modules.
The complete boundary is recorded in
[`platform_support.md`](platform_support.md).

The Windows job validates the IOCP completion path and must not freeze a
select-based backend as an accepted target. Its Debug configuration runs CTest
with `-C Debug`, installs the package, and configures, builds, and executes the
external consumer through `find_package(GameNetCore)`. The separate Windows
Release job builds and executes the full CTest suite with `-C Release`, then
installs the Release libraries and configures, builds, and executes a second
external consumer against that Release package.

## Required Local Equivalent

When a CMake toolchain is available locally, use:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON
python3 tools/verify_ctest_inventory.py --test-dir build --expected-total 120 --output build/ctest-inventory.json
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 60 --output-junit "$PWD/build/ctest-junit.xml" --output-log "$PWD/build/ctest.log"

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TSAN=ON
python3 tools/verify_ctest_inventory.py --test-dir build-tsan --expected-total 120 --expect-label threading=93 --output build-tsan/ctest-inventory.json
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure -L threading --timeout 60 --output-junit "$PWD/build-tsan/ctest-junit.xml" --output-log "$PWD/build-tsan/ctest.log"

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DGAMENET_BUILD_TESTING=ON
python3 tools/verify_ctest_inventory.py --test-dir build-release --expected-total 120 --output build-release/ctest-inventory.json
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure --timeout 60 --output-junit "$PWD/build-release/ctest-junit.xml" --output-log "$PWD/build-release/ctest.log"
```

On Windows with Visual Studio installed, the local equivalent is:

```powershell
cmake -S . -B build-windows -G "Visual Studio 18 2026" -A x64 -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TLS=OFF -DGAMENET_ENABLE_EXPERIMENTAL=OFF
python tools/verify_ctest_inventory.py --test-dir build-windows --config Debug --expected-total 120 --output build-windows/ctest-inventory.json
cmake --build build-windows --config Debug --parallel
ctest --test-dir build-windows -C Debug --output-on-failure --timeout 30 --output-junit "$pwd/build-windows/ctest-junit.xml" --output-log "$pwd/build-windows/ctest.log"
cmake --install build-windows --config Debug --prefix "$pwd/build-windows/_install"
cmake -S tests/cmake/install_consumer -B build-windows-install-consumer -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$pwd/build-windows/_install"
python tools/verify_ctest_inventory.py --test-dir build-windows-install-consumer --config Debug --expected-total 2 --output build-windows-install-consumer/ctest-inventory.json
cmake --build build-windows-install-consumer --config Debug --parallel
ctest --test-dir build-windows-install-consumer -C Debug --output-on-failure --timeout 60 --output-junit "$pwd/build-windows-install-consumer/ctest-junit.xml" --output-log "$pwd/build-windows-install-consumer/ctest.log"

cmake -S . -B build-windows-release -G "Visual Studio 18 2026" -A x64 -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TLS=OFF -DGAMENET_ENABLE_EXPERIMENTAL=OFF
python tools/verify_ctest_inventory.py --test-dir build-windows-release --config Release --expected-total 120 --output build-windows-release/ctest-inventory.json
cmake --build build-windows-release --config Release --parallel
ctest --test-dir build-windows-release -C Release --output-on-failure --timeout 30 --output-junit "$pwd/build-windows-release/ctest-junit.xml" --output-log "$pwd/build-windows-release/ctest.log"
cmake --install build-windows-release --config Release --prefix "$pwd/build-windows-release/_install"
cmake -S tests/cmake/install_consumer -B build-windows-release-install-consumer -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$pwd/build-windows-release/_install"
python tools/verify_ctest_inventory.py --test-dir build-windows-release-install-consumer --config Release --expected-total 2 --output build-windows-release-install-consumer/ctest-inventory.json
cmake --build build-windows-release-install-consumer --config Release --parallel
ctest --test-dir build-windows-release-install-consumer -C Release --output-on-failure --timeout 60 --output-junit "$pwd/build-windows-release-install-consumer/ctest-junit.xml" --output-log "$pwd/build-windows-release-install-consumer/ctest.log"
```

Run the scope guard before the CMake commands. On Windows hosts where the
Microsoft Store `python.exe` alias is inactive, use `py -3` instead:

```powershell
py -3 tests\scope\test_scope_guard.py
py -3 tests\scope\test_intent_consistency.py
py -3 tests\scope\test_intent_metadata.py
py -3 tools\verify_migration_provenance.py
py -3 tests\scope\test_intent_semantics.py
py -3 tools\check_scope_boundaries.py
py -3 tests\cmake\test_sanitizer_flags.py
py -3 tests\cmake\test_install_package_contract.py
py -3 tests\cmake\test_packet_framer_fuzz_contract.py
py -3 tests\cmake\test_core_benchmark_contract.py
py -3 tests\cmake\test_phase4_benchmark_contract.py
py -3 tests\cmake\test_capacity_profile_contract.py
py -3 tests\cmake\test_logger_thread_contract.py
py -3 tests\cmake\test_event_loop_contracts.py
py -3 tests\cmake\test_event_loop_thread_pool_contracts.py
py -3 tests\cmake\test_migration_status_contract.py
py -3 tests\cmake\test_msvc_utf8_contract.py
py -3 tests\cmake\test_platform_backend_contract.py
py -3 tests\cmake\test_build_governance_contract.py
py -3 tests\cmake\test_tcp_lifecycle_contracts.py
py -3 tests\cmake\test_tcp_connection_context_contract.py
py -3 tests\cmake\test_tcp_connection_thread_contract.py
py -3 tests\cmake\test_timer_queue_contracts.py
py -3 tests\cmake\test_threading_gate_contracts.py
py -3 tests\cmake\test_windows_iocp_milestone_contract.py
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
py -3 tests\cmake\test_release_safe_tests.py
py -3 tests\ci\test_workflow_jobs.py
py -3 tests\ci\test_long_soak_workflow.py
py -3 tests\ci\test_core_benchmark_workflow.py
py -3 tests\ci\test_phase4_benchmark_workflow.py
```

The local command and CI workflow should stay aligned so test results remain
comparable across developer machines and remote validation.
