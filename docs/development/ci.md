# Continuous Integration

The first CI gate keeps the migration focused on the Reactor/TCP foundation.

## Scope

The `ci` workflow validates:

- CMake configure with C++23 and testing enabled.
- Core library build.
- Minimal examples build.
- Unit, contract, and integration tests through CTest.

It keeps deferred modules disabled:

- TLS
- experimental transport
- HTTP, WebSocket, RPC, UDP, KCP, PMTU/FEC, metrics, and game pipeline modules

Those deferred intents remain design assets until the core target has stable
builds, tests, and examples.

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

The local command and CI workflow should stay aligned so test results remain
comparable across developer machines and remote validation.
