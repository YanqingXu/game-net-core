# Build Profiles

The repository provides named CMake presets for repeatable build intent. They
do not change the production backend selector or constitute performance evidence.

| Display name | Configure preset | Purpose |
| --- | --- | --- |
| PortableRelease | `portable-release` | Release build without host ISA tuning; use for portable packages. |
| NativeTunedRelease | `native-tuned-release` | Host-specific Release (`-march=native` or MSVC AVX2); deploy only to compatible CPUs. |
| PGOGenerate | `pgo-generate` | Instrumented training build with benchmarks. |
| PGORelease | `pgo-release` | Reconfigures the same build tree to consume same-toolchain training data. |
| Sanitizer | `sanitizer` | Debug ASan/UBSan on GCC/Clang and ASan on MSVC. |
| BenchmarkInstrumented | `benchmark-instrumented` | Release benchmarks with frame pointers and profiling debug information. |

Basic use:

```text
cmake --preset portable-release
cmake --build --preset portable-release
ctest --preset portable-release
```

Native tuning is explicit and never inherited by PortableRelease. A native or
PGO binary is a machine-fleet artifact, not a portable release package.

PGO is a two-pass workflow in one build tree:

```text
cmake --preset pgo-generate
cmake --build --preset pgo-generate
# Run the preregistered representative benchmark/profile workload here.
cmake --preset pgo-release
cmake --build --preset pgo-release
```

Do not reuse profiles across compiler versions, source revisions, flags or
workload definitions. The GCC/Clang profiles use `out/pgo-data`; MSVC retains
its linker-managed profile database in the shared build tree. PGO and sanitizer
instrumentation are rejected when combined.

`BenchmarkInstrumented` deliberately keeps symbols/frame pointers and is not a
claim that instrumentation overhead equals deployment performance. Promotion
still requires the HP0 clean exact-commit fixed-lab ledger and 5%/3% rules.
