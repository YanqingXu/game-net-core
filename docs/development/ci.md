# Continuous Integration

The first CI gate keeps the migration focused on the Reactor/TCP foundation.

## Scope

The `ci` workflow validates:

- CMake configure with C++23 and testing enabled.
- Scope boundary guard for deferred modules and legacy `mini_trantor` symbols.
- Intent consistency guard for active module paths and support intents.
- Intent metadata contract guard for complete active/deferred/legacy catalogs,
  target/promotion gates, and stale source-project wording in active intents.
- MSVC `/utf-8` and `/FS` compile-option guard so Windows builds parse UTF-8
  source/header comments consistently and parallel Debug builds serialize PDB
  writes safely.
- Short generated CMake test target names so verbose contract test identifiers
  remain visible in CTest while MSBuild intermediate `.tlog` paths stay below
  Windows path-length limits.
- Windows IOCP milestone guard for
  `docs/development/windows_iocp_milestone.md`, keeping Windows support tied
  to the IOCP backend and the Windows MSVC workflow gate.
- Windows IOCP data-path guard for the planned overlapped operation layer,
  socket extension helpers, and completion metadata translation.
- EventLoop contract guard for owner-thread scheduling, quit, and bounded
  future wait helper coverage.
- Logger thread-contract guard for concurrent emission, runtime callback
  replacement, callback snapshot/re-entry semantics, and TSan selection.
- EventLoopThreadPool contract guard for queued-work race/soak coverage.
- TimerQueue contract guard for ready-timer cancellation races.
- Threading gate contract guard for the race-oriented TSan test selection.
- TcpConnection thread-contract guard for atomic public state observation and
  owner-loop-only callback/socket-option mutation.
- Core benchmark contract guard for the default-off, non-CTest target,
  versioned JSON schema, three scenarios, platform resource sampling, and docs.
- Long-soak workflow guard for the non-default repeated threading contract gate.
- Manual core-benchmark workflow guard for paired Linux/Windows Release raw
  JSON artifacts without push/PR triggers or performance thresholds.
- Sanitizer CMake contract check so ASan/UBSan and TSan flags apply to the
  core target itself as well as dependent tests/examples.
- Core library build.
- Minimal examples build.
- Unit, contract, and integration tests through CTest.
- Install and `find_package(GameNetCore)` consumer verification.
- ASan/UBSan Debug build and CTest suite on Linux.
- TSan Debug build and `threading`-labeled CTest suite on Linux.
- Release build and CTest suite on Linux.
- Windows MSVC Debug build, CTest suite, and install/package consumer
  verification through the IOCP backend.

It keeps deferred modules disabled:

- TLS
- experimental transport
- HTTP, WebSocket, RPC, UDP, KCP, PMTU/FEC, metrics, and game pipeline modules

Those deferred intents remain design assets until the core target has stable
builds, tests, and examples.

The scope guard runs before CMake configure:

```bash
python3 tests/scope/test_scope_guard.py
python3 tests/scope/test_intent_consistency.py
python3 tests/scope/test_intent_metadata.py
python3 tools/check_scope_boundaries.py
python3 tests/cmake/test_sanitizer_flags.py
python3 tests/cmake/test_install_package_contract.py
python3 tests/cmake/test_core_benchmark_contract.py
python3 tests/cmake/test_logger_thread_contract.py
python3 tests/cmake/test_event_loop_contracts.py
python3 tests/cmake/test_event_loop_thread_pool_contracts.py
python3 tests/cmake/test_migration_status_contract.py
python3 tests/cmake/test_msvc_utf8_contract.py
python3 tests/cmake/test_platform_backend_contract.py
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
```

It fails the workflow if active intents use legacy `mini/net` paths, or if
`mini/` includes, `mini::` namespaces, inactive
`gamenet::protocol` / `gamenet::transport` / `gamenet::game` /
`gamenet::experimental` paths, or deferred high-level module names appear in
the active implementation and test surface. Promoting a Phase 4 component must
update the active scope and migration status in the same change.

## Remote Evidence Boundary

Record immutable validation evidence instead of a self-referential current HEAD:

- Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`.
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
No later commit may be described as validated until a new run id, commit,
date, and complete job result are recorded here.

The ASan/UBSan job uses:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
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
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure -L threading --timeout 30
```

The Release job uses:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DGAMENET_BUILD_TESTING=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

The C++ tests use the `tests/support/TestAssert.h` helper instead of standard
`assert`, so contract checks remain active when Release builds define `NDEBUG`.
Debug, ASan/UBSan, and Release jobs are all test-execution gates.

## Non-Default Long Soak

The `long-soak` workflow is manual-only through `workflow_dispatch`; it is not
attached to `push` or `pull_request`. It complements the regular CI and TSan
jobs by rebuilding the core target and repeatedly running the `threading`
contract slice. Its current dispatch input defaults to repeat 50 and a
60-second per-test timeout, matching the Phase 3.5 evidence gate:

Long-soak repository guards include `tests/cmake/test_event_loop_contracts.py`
so the manual soak guard surface stays aligned with the ordinary `ci` workflow
before CMake configure.

```bash
cmake -S . -B build-long-soak -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TLS=OFF -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-long-soak --parallel
ctest --test-dir build-long-soak --output-on-failure -L threading --repeat until-fail:<repeat> --timeout <timeout_seconds>
```

Use it for phase hardening evidence when mixed-timing lifecycle contracts need
more iteration than the ordinary PR gate should spend.

Current remote evidence: run `29077148022`, job `86311227712`, commit
`a7fd77cbd2140041cebb3f900d5c609fafc2adad`, repeat 50, timeout 60 seconds,
completed successfully at 2026-07-10T08:04:12Z. CTest reported 46/46
threading-labeled tests passed and total real test time of 1632.47 seconds;
the complete workflow job took 28m27s. The command recorded in the log was:

```bash
ctest --test-dir build-long-soak --output-on-failure -L threading --repeat until-fail:50 --timeout 60
```

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
thread-safety contract now expands the present threading slice to 46 tests.

Current local Windows preflight on 2026-07-10 passes 67/67 Debug tests in
43.57 seconds and 67/67 Release tests in 37.38 seconds. The repaired
`contract.tcp_client.test_tcp_client_repeated_connect` passes 50 repeats in
32.41 seconds with a 5-second per-test timeout. The full current-worktree
46-test threading
slice across 5 repeats passes in 176.90 seconds with a 15-second per-test timeout.
The Release install plus external `find_package(GameNetCore)` / `GameNet::core`
consumer also configures, builds, and exits successfully. These local results
supplement the same-SHA remote CI and repeat-50 evidence above.

## Remote Core Benchmark Evidence

Manual `core-benchmark` run `29077151229` completed successfully on commit
`a7fd77cbd2140041cebb3f900d5c609fafc2adad` at 2026-07-10T07:37:15Z. It
published two SHA-bound Release artifacts:

- `core-benchmark-linux-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad`
- `core-benchmark-windows-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad`

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
callbacks. Windows identifies backend `iocp` and completion mode
`single_get_queued_completion_status`; its one/two-worker snapshots are
16.188/26.110 MiB/s with P99 162.8/151.3 us, the connection working-set delta
is 18,137,088 bytes (70,848 bytes per connection), and slow-client working-set
delta is 67,493,888 bytes with four high-water callbacks. These are raw
environment-specific snapshots, not performance thresholds.

## Install Package Gate

The Linux Debug job verifies that the split core target is consumable from
outside the source tree:

```bash
cmake --install build --prefix "$PWD/build/_install"
cmake -S tests/cmake/install_consumer -B build-install-consumer \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$PWD/build/_install"
cmake --build build-install-consumer --parallel
```

The consumer fixture must use `find_package(GameNetCore REQUIRED)` and link only
against `GameNet::core`. This keeps the package contract focused on the current
Reactor/TCP foundation target.

The Windows MSVC job runs the same package consumer gate after installing the
Debug build from `build-windows`, using the Visual Studio generator and `x64`
platform so the installed `GameNet::core` target is verified on the IOCP
backend.

## Current Platform Gate

The active CI gate runs on `ubuntu-24.04` and `windows-latest`.

The Windows job validates the IOCP completion path and must not freeze a
select-based backend as an accepted target. It uses the Visual Studio generator,
builds Debug, runs CTest with `-C Debug`, installs the package, and builds the
external install consumer through `find_package(GameNetCore)`.

## Required Local Equivalent

When a CMake toolchain is available locally, use:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure -L threading --timeout 30

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DGAMENET_BUILD_TESTING=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

On Windows with Visual Studio installed, the local equivalent is:

```powershell
cmake -S . -B build-windows -G "Visual Studio 18 2026" -A x64 -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TLS=OFF -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-windows --config Debug --parallel
ctest --test-dir build-windows -C Debug --output-on-failure --timeout 10
cmake --install build-windows --config Debug --prefix "$pwd/build-windows/_install"
cmake -S tests/cmake/install_consumer -B build-windows-install-consumer -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$pwd/build-windows/_install"
cmake --build build-windows-install-consumer --config Debug --parallel
```

Run the scope guard before the CMake commands. On Windows hosts where the
Microsoft Store `python.exe` alias is inactive, use `py -3` instead:

```powershell
py -3 tests\scope\test_scope_guard.py
py -3 tests\scope\test_intent_consistency.py
py -3 tests\scope\test_intent_metadata.py
py -3 tools\check_scope_boundaries.py
py -3 tests\cmake\test_sanitizer_flags.py
py -3 tests\cmake\test_install_package_contract.py
py -3 tests\cmake\test_core_benchmark_contract.py
py -3 tests\cmake\test_logger_thread_contract.py
py -3 tests\cmake\test_event_loop_contracts.py
py -3 tests\cmake\test_event_loop_thread_pool_contracts.py
py -3 tests\cmake\test_migration_status_contract.py
py -3 tests\cmake\test_msvc_utf8_contract.py
py -3 tests\cmake\test_platform_backend_contract.py
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
```

The local command and CI workflow should stay aligned so test results remain
comparable across developer machines and remote validation.
