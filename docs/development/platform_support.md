# Platform and Build Support

This document defines the build boundary for the pre-1.0 `game-net-core`
candidate line. A successful CMake configure is a support claim, so unsupported
platforms and unimplemented options fail explicitly instead of borrowing an
unrelated backend or producing an empty feature toggle.

## Support Matrix

| Target system | Tier | Backend | Current claim |
|---|---|---|---|
| Linux | Tier 1 | epoll | Reference platform for release, sanitizers, performance regression, and 24/72-hour endurance evidence |
| Windows | Tier 2 until M3 | IOCP | Required functional, lifecycle, Debug/Release, benchmark, install, and package-consumer coverage |
| macOS | Unsupported | none | CMake configure fails |
| FreeBSD, OpenBSD, NetBSD, and other BSD variants | Unsupported | none | CMake configure fails |
| Any other target system | Unsupported | none | CMake configure fails |

Tier 2 does not mean that the Windows implementation may be skipped. The IOCP
path remains a required CI gate. It means the current release and
long-duration reference claim is Linux, while Windows still needs the M3
promotion work: batched completions, proven
overlapped-operation ownership and cancellation at scale, wakeup coalescing,
an AcceptEx pool, bounded read-buffer ownership, and reviewed
capacity/performance evidence. Synchronous submit-error convergence is already
covered by the M1 IOCP contract; that correctness fix alone does not promote
the backend to Tier 1.

No Unix-family fallback exists. In particular, macOS and BSD targets must not
compile the Linux epoll, eventfd, or Linux socket implementation merely because
they are not Windows.

## Library Form and ABI

All installed libraries are explicitly static:

- `GameNet::core`
- `GameNet::protocol`
- `GameNet::transport`
- `GameNet::game_session`
- `GameNet::game_logic`
- `GameNet::broadcast`

`BUILD_SHARED_LIBS=ON` is rejected during configure. The project does not yet
define DLL/shared-object visibility, symbol export, runtime-library,
toolchain, standard-library, or cross-configuration compatibility contracts.
It therefore makes no binary ABI compatibility promise before version 1.0.
Consumers must rebuild against each 0.x release and use the same supported
toolchain/configuration family throughout one final binary.

The source-level classes recorded in `api/public_api_manifest.json` are a
separate promise. A stable source contract in the 0.3 line does not imply a
stable binary ABI.

## Deferred Options

The following cache options are retained so existing `OFF`-explicit build
commands remain valid:

| Option | Supported value | `ON` behavior |
|---|---|---|
| `GAMENET_ENABLE_TLS` | `OFF` | Configure fails because TLS is not implemented in the active target graph |
| `GAMENET_ENABLE_EXPERIMENTAL` | `OFF`, or `ON` on Linux | `ON` builds the non-installed IOE-X1–X10 io_uring Engine, EventLoop pump, single-connection driver, shared-Pump Hub/listener, semantic adapter, direct contracts, and optional fixed listener-comparison benchmark; Windows rejects it |

An option that has no implementation must not configure successfully. The
Linux-only IOE-X1–X10 io_uring target is the sole active experimental module;
it does not replace epoll and is not installed. TLS, UDP, KCP, and other
experimental work still require promoted intent, ownership/threading
contracts, targets, and direct tests before an enabling value can be accepted.

## Supported Configuration Examples

Linux:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DGAMENET_BUILD_TESTING=ON \
  -DGAMENET_ENABLE_TLS=OFF \
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows:

```powershell
cmake -S . -B build-windows -A x64 `
  -DBUILD_SHARED_LIBS=OFF `
  -DGAMENET_BUILD_TESTING=ON `
  -DGAMENET_ENABLE_TLS=OFF `
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-windows --config Release --parallel
ctest --test-dir build-windows -C Release --output-on-failure
```

The default for all three boundary options is `OFF`; spelling them out is
recommended in reproducible CI and evidence commands.

The dedicated IOE-X1–X10 Engine, EventLoop-pump,
single-connection-driver, shared-Pump Hub/listener, semantic adapter,
fixed-capacity, graceful, bounded cross-thread, and listener contract
configuration is Linux-only:

```bash
cmake -S . -B build-io-uring \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGAMENET_BUILD_TESTING=ON \
  -DGAMENET_ENABLE_EXPERIMENTAL=ON
cmake --build build-io-uring --target gamenet_io_uring_contracts --parallel
ctest --test-dir build-io-uring \
  -R '^contract.io_engine.test_io_uring_(completion_engine|event_loop_pump|tcp_connection_driver|tcp_connection_hub|tcp_connection_hub_capacity|tcp_listener|tcp_connection_adapter)$' \
  --output-on-failure
```

## Contract Enforcement

`tests/cmake/test_build_governance_contract.py` checks:

- the root platform allow-list and explicit Core backend selection;
- configure-time rejection for shared libraries, TLS, unsupported Windows
  experimental requests, and any unimplemented option;
- the default-off, Linux-only, non-installed IOE-X1–X10 io_uring target;
- explicit `STATIC` declarations for every installed library target;
- this support matrix, README/CI documentation, and workflow registration.

The guard runs before configure in ordinary Linux/Windows CI and the manual
long-soak workflow.
