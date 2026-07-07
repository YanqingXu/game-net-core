# Continuous Integration

The first CI gate keeps the migration focused on the Reactor/TCP foundation.

## Scope

The `ci` workflow validates:

- CMake configure with C++23 and testing enabled.
- Scope boundary guard for deferred modules and legacy `mini_trantor` symbols.
- Intent consistency guard for active module paths and support intents.
- MSVC `/utf-8` and `/FS` compile-option guard so Windows builds parse UTF-8
  source/header comments consistently and parallel Debug builds serialize PDB
  writes safely.
- Windows IOCP milestone guard for
  `docs/development/windows_iocp_milestone.md`, keeping Windows support tied
  to the IOCP backend and the Windows MSVC workflow gate.
- Windows IOCP data-path guard for the planned overlapped operation layer,
  socket extension helpers, and completion metadata translation.
- EventLoopThreadPool contract guard for queued-work race/soak coverage.
- TimerQueue contract guard for ready-timer cancellation races.
- Sanitizer CMake contract check so ASan/UBSan and TSan flags apply to the
  core target itself as well as dependent tests/examples.
- Core library build.
- Minimal examples build.
- Unit, contract, and integration tests through CTest.
- Install and `find_package(GameNetCore)` consumer verification.
- ASan/UBSan Debug build and CTest suite on Linux.
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
python3 tools/check_scope_boundaries.py
python3 tests/cmake/test_sanitizer_flags.py
python3 tests/cmake/test_install_package_contract.py
python3 tests/cmake/test_event_loop_thread_pool_contracts.py
python3 tests/cmake/test_migration_status_contract.py
python3 tests/cmake/test_msvc_utf8_contract.py
python3 tests/cmake/test_platform_backend_contract.py
python3 tests/cmake/test_tcp_lifecycle_contracts.py
python3 tests/cmake/test_tcp_connection_context_contract.py
python3 tests/cmake/test_timer_queue_contracts.py
python3 tests/cmake/test_windows_iocp_milestone_contract.py
python3 tests/cmake/test_windows_iocp_data_path_contract.py
python3 tests/cmake/test_release_safe_tests.py
python3 tests/ci/test_workflow_jobs.py
```

It fails the workflow if active intents use legacy `mini/net` paths, or if
`mini/` includes, `mini::` namespaces, inactive
`gamenet::protocol` / `gamenet::transport` / `gamenet::game` /
`gamenet::experimental` paths, or deferred high-level module names appear in
the active implementation and test surface. Promoting a Phase 4 component must
update the active scope and migration status in the same change.

The ASan/UBSan job uses:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
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
py -3 tools\check_scope_boundaries.py
py -3 tests\cmake\test_sanitizer_flags.py
py -3 tests\cmake\test_install_package_contract.py
py -3 tests\cmake\test_event_loop_thread_pool_contracts.py
py -3 tests\cmake\test_migration_status_contract.py
py -3 tests\cmake\test_msvc_utf8_contract.py
py -3 tests\cmake\test_platform_backend_contract.py
py -3 tests\cmake\test_tcp_lifecycle_contracts.py
py -3 tests\cmake\test_tcp_connection_context_contract.py
py -3 tests\cmake\test_timer_queue_contracts.py
py -3 tests\cmake\test_windows_iocp_milestone_contract.py
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
py -3 tests\cmake\test_release_safe_tests.py
py -3 tests\ci\test_workflow_jobs.py
```

The local command and CI workflow should stay aligned so test results remain
comparable across developer machines and remote validation.
