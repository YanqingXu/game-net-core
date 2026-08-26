# HP8 Backend, Build Profile, And Promotion Review

Date: 2026-08-25

Decision: backend/promotion `DEFER`; release `NO-RELEASE`

## Backend Review

HP8 requires a comparable post-user-data-path replay of epoll, IOCP and
io_uring. That state does not exist: HP0 has no clean native fixed-lab ledger,
and HP1-HP6 remain experimental/deferred rather than production-integrated.
Comparing today's backends would not answer the planned question.

Linux remains epoll, Windows remains IOCP, and the explicit Linux io_uring
package remains unchanged. Multishot accept/recv, provided buffers, registered
files, send bundles and SQPOLL were not started. No selector/default/API changed.

## Delivered Build Profiles

`CMakePresets.json` now exposes PortableRelease, NativeTunedRelease,
PGOGenerate, PGORelease, Sanitizer and BenchmarkInstrumented. CMake configuration
passed for all six on Windows MSVC. Generated projects showed:

- PortableRelease contains no AVX2 host tuning;
- NativeTunedRelease contains MSVC AVX2 tuning;
- PGOGenerate/PGORelease contain `PGINSTRUMENT`/`PGOPTIMIZE` respectively and
  share a build tree;
- BenchmarkInstrumented retains frame pointers and program-database information;
- sanitizer/PGO combination is rejected by configuration.

The PortableRelease `gamenet_core` target built successfully. Linux flag/runtime
validation remains part of later cross-platform CI, not claimed by this Windows
review.

## Final Working-Tree Validation

The completed HP1-HP8 prestudy/review tree built under Windows MSVC Release and
Debug AddressSanitizer. The complete CTest matrix passed 137/137 in both
configurations, including all seven `experimental`/`prestudy` contracts and the
unchanged production regression suite. The 44 Python API, CMake, scope, CI and
intent-governance guards also passed; intent semantics report 44 active targets
and 210 explicit verification paths.

One first Release run at eight-way test parallelism observed an existing
loopback-client timing assertion in
`test_network_logic_split_profile`; the exact test immediately passed alone and
the complete suite passed at four-way parallelism. An initial unrestricted
parallel ASan build also hit an MSVC PDB writer `C1090`; the incremental build
passed at parallelism two. Neither observation required a source change or
altered the HP decisions.

## Deployment Guidance

`performance_deployment_guide.md` records measurement-first CPU affinity, NUMA,
IRQ/RSS/RPS/XPS, socket-buffer, TCP_NODELAY and SO_REUSEPORT boundaries with
Linux kernel and Microsoft primary documentation. SO_REUSEPORT remains behind
the existing accept-topology evidence gate.

## Promotion And Release

No HP0 matrix replay or independent architecture/API review can be completed
without the missing native evidence and integrated candidates. The final
decision is `DEFER` / `NO-RELEASE`. Project version remains 0.3.0; no tag,
package, release or future version promise is created.
