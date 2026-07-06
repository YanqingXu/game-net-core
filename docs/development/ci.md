# Continuous Integration

The first CI gate keeps the migration focused on the Reactor/TCP foundation.

## Scope

The `ci` workflow validates:

- CMake configure with C++23 and testing enabled.
- Scope boundary guard for deferred modules and legacy `mini_trantor` symbols.
- Sanitizer CMake contract check so ASan/UBSan and TSan flags apply to the
  core target itself as well as dependent tests/examples.
- Core library build.
- Minimal examples build.
- Unit, contract, and integration tests through CTest.
- ASan/UBSan Debug build and CTest suite on Linux.
- Release compile gate on Linux.

It keeps deferred modules disabled:

- TLS
- experimental transport
- HTTP, WebSocket, RPC, UDP, KCP, PMTU/FEC, metrics, and game pipeline modules

Those deferred intents remain design assets until the core target has stable
builds, tests, and examples.

The scope guard runs before CMake configure:

```bash
python3 tests/scope/test_scope_guard.py
python3 tools/check_scope_boundaries.py
python3 tests/cmake/test_sanitizer_flags.py
python3 tests/ci/test_workflow_jobs.py
```

It fails the workflow if `mini/` includes, `mini::` namespaces, inactive
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
```

Release still builds the test executables, but it does not run CTest because the
current C++ tests use `assert` for contract checks and Release builds define
`NDEBUG`. Debug and ASan/UBSan jobs remain the authoritative test-execution
gates until the tests are migrated to a Release-safe assertion helper.

## Current Platform Gate

The first gate runs on `ubuntu-24.04`.

Windows build validation is intentionally deferred until the core Linux gate is
green and the Windows socket/runtime path is reviewed under the same
intent-driven rules. Adding Windows CI should be its own focused migration step,
with failures treated as platform contract gaps rather than an expansion of the
module scope.

## Required Local Equivalent

When a CMake toolchain is available locally, use:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the scope guard before the CMake commands. On Windows hosts where the
Microsoft Store `python.exe` alias is inactive, use `py -3` instead:

```powershell
py -3 tests\scope\test_scope_guard.py
py -3 tools\check_scope_boundaries.py
py -3 tests\cmake\test_sanitizer_flags.py
py -3 tests\ci\test_workflow_jobs.py
```

The local command and CI workflow should stay aligned so test results remain
comparable across developer machines and remote validation.
