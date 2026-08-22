# M2 Internal Candidate Convergence Checkpoint — 2026-08-22

> **Decision: `STOPPED / NO-PROMOTION`.** This document is an execution
> checkpoint, not an M2 closure record, internal release, or waiver. The name
> `v0.3.0-internal-candidate.1` is reserved but was not produced.
> The run names and durations in the evidence sections below preserve the
> historical v1 policy. The current v2 restart policy is 1h/3h; it does not
> retroactively promote the cancelled v1 result.

## Identity and scope

| Field | Value |
| --- | --- |
| Exact promotion candidate | `a89e2b0f13242a4cd4b49093f562b93adcd509a4` |
| Repository/ref | `YanqingXu/game-net-core`, candidate selected from `main` |
| Production backends | Linux/epoll; Windows/IOCP |
| Stable API | Reviewed v0.3 surface, zero diff in candidate CI |
| Experimental scope | IOE-X1–X10 remained default-off, Linux-only, non-installed |
| Stop reason | User-directed task convergence on 2026-08-22 |

No Core implementation, owner loop, callback, cross-thread admission, or
lifetime contract changed while producing this checkpoint. The only candidate
fix-forwards repaired evidence workflow execution: `0271005` restored the
io_uring producer command, `1755ae4` pinned CMake 4.4.2 for the Windows capacity
runner, and `a89e2b0` corrected the pinned generator check. Earlier incomplete
attempts are not candidate evidence.

## Passing exact-commit evidence

| Gate | Result and retained identity |
| --- | --- |
| Main CI | Run [`32515776695`](https://github.com/YanqingXu/game-net-core/actions/runs/32515776695), attempt 1, 7/7 success. Six producers cover Linux Debug, ASan/UBSan, TSan, Release, Windows Debug/IOCP, and Windows Release/IOCP; aggregate artifact digest `sha256:4f50071c2db46fddd873b46d228a2f15fac158d83bc579f35d0838bf0a85e31a`. The independently regenerated `gamenet.ci_evidence_set.v1` is successful and has SHA-256 `a9d393da58f1fb531996f76907a14b011741ed5e3fabb5dcdc0c7608f5c8e036`. |
| Dedicated capacity | Run [`32515817027`](https://github.com/YanqingXu/game-net-core/actions/runs/32515817027), attempt 1, Linux + Windows + pair success at 100,000 endpoint attempts and 10,000 probes. Pair artifact digest `sha256:5d69b4452140da34105ba78b6fdba91af1695d4d760f10c5c20f86b4abc5d03f`; independent manifest SHA-256 `5bf1922ff53025748fa01e0026bd05bde2fd3c8bf03a1c97be7b98219d05d972`. |
| Candidate capacity | Run [`32515907929`](https://github.com/YanqingXu/game-net-core/actions/runs/32515907929), attempt 1, Linux + Windows + pair success at 10,000 endpoint attempts, 500 probes, three repetitions. Pair artifact digest `sha256:05eb815dbae12660d473a3eb0fcff06b011402faa1347d70c4c77d5913d18a7c`; independent manifest SHA-256 `f9dd29d1fe3efa179c40a0cd139a493b7cbeafc0ce9e05f83767461c836d4df0`. |
| Paired benchmark | Run [`32515904248`](https://github.com/YanqingXu/game-net-core/actions/runs/32515904248), attempt 1, Linux/epoll + Windows/IOCP + pair success. Performance and Core capacity regression matrices passed. Linux topology decision remained `retain_single_listener`. Pair artifact digest `sha256:a64fcd16798a63c92bff91031a360dc59eff80a7ab0b384b0bb47cf9d1554795`; independent manifest SHA-256 `183b7a4a8371350c55d288ba1d42d4ccbe7f8322863071ee00bfb3cc258254d7`. |
| Repeat and fault coverage | Run [`32515900974`](https://github.com/YanqingXu/game-net-core/actions/runs/32515900974), attempt 1, success. The 102-test threading selection passed 50 times (5,100/5,100 executions, 2,336.23 test-seconds); the 12-test Pipeline/Broadcast selection passed 50 times (600/600, 41.89 test-seconds). The threading selection includes the fault-injection integration test. Artifact digest `sha256:d7d06970f076702020f3e7c074bcf81e6e497c604e78d57b6ef2416136c9bebd`; independently regenerated evidence SHA-256 values are `53713366f1bd116af20d127ddcece7ee5a3823ba355b3a630a3c05dfd0b20d14` and `b9c80b477588ca023b55daefa7d8e9e7da4a6e3e42414027c661374790f7d14e`. |

Every row above binds candidate SHA, run ID, run attempt, platform/backend, and
canonical artifact identity. These passing rows remain useful exact-commit
evidence, but they do not override the missing endurance gate.

## Package preflight, not a release

Isolated exact-candidate builds produced draft local archives and then built
and ran both installed consumers from the extracted binary packages:

| Draft artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `game-net-core-v0.3.0-internal-candidate.1-source.tar.gz` | 1,132,920 | `afdd77818a9f718de316b8570d6dc18e02ef537859650b33e4fae2733f370aac` |
| `game-net-core-v0.3.0-internal-candidate.1-source.zip` | 1,566,158 | `626208c755f3c748bed2b8e3ae5bd6cc9675a398208548bc36fef203b86739fc` |
| `game-net-core-v0.3.0-internal-candidate.1-linux-x86_64.tar.gz` | 505,611 | `fb19bd6862ab33f1876bc1712a670a6d9bb90a642b3b9003addab93f37d0a384` |
| `game-net-core-v0.3.0-internal-candidate.1-windows-x86_64.zip` | 2,223,337 | `f60d25fa92a223bd6dbb9bbc1c97a5cde57a9d483f67804601d742f27f969bce` |

Both source archives matched all 561 tracked files. Each binary archive had 65
installed files plus README, license, and third-party notice; archive path
safety validation passed, and its two clean
`find_package(GameNetCore 0.3.0 EXACT)` consumers passed. Linux used GCC 13.3.0
and CMake 3.28.3; Windows used MSVC 19.51.36256, Windows SDK 10.0.26100.0, and
the SHA-256-pinned CMake 4.4.2 distribution.

These archives remain in the ignored local `out/` workspace only. A final
SBOM, evidence index, and `SHA256SUMS` were intentionally not emitted after the
endurance gate became incomplete. No tag, GitHub Release, or externally
adoptable artifact was published. The repository remains all-rights-reserved.

## Interrupted endurance evidence

Candidate-24h run
[`32516260909`](https://github.com/YanqingXu/game-net-core/actions/runs/32516260909),
attempt 1, successfully completed identity validation, repository guards,
Release build, test inventory validation, and exact candidate-10k capacity
revalidation. Its uninterrupted test step started at `2026-08-21T19:43:36Z`
and was cancelled at task convergence before the required 86,400 seconds:

| Observation at cancellation | Value |
| --- | ---: |
| Child elapsed time | 21,073.016 seconds (5h 51m 13.016s) |
| Completed cycles | 17,978 |
| Each profile | 17,978 (`abrupt_peer_reset`, `callback_exception`, `output_overload`, `healthy_recovery`, `forced_shutdown`) |
| First RSS | 4,964,352 bytes |
| Last / maximum RSS | 14,512,128 bytes |
| RSS growth | 9,547,776 bytes |
| Configured RSS / growth budgets | 536,870,912 / 67,108,864 bytes |

The partial artifact is retained as
`production-endurance-candidate-24h-linux-production-endurance-a89e2b0f13242a4cd4b49093f562b93adcd509a4-32516260909-1`,
digest `sha256:dccd5c8d352529b1dea52f9fbe14d246c359773927fc6e4ca0fe1fb377bd116f`.
Its wrapper status is `cancelled`; the checkpoint status is still `running`;
the current-result and promotion verification steps were skipped. Because the
fault log continued to flush after the cancelled wrapper manifest was written,
the uploaded log has 9,776,125 bytes and SHA-256
`34e21b6603afd2f29401d94dc59c681c15a59cb88e7c0520d7560a7055c04659`,
while the cancelled wrapper declares the earlier 9,775,828-byte snapshot. This
expected cancellation race makes the artifact unsuitable as passing evidence.

Release-72h was never dispatched. No endurance waiver was requested or used.

## Convergence and restart rule

The temporary Linux and Windows self-hosted runners were idle before cleanup,
then their services and GitHub registrations were removed. The dedicated WSL
keepalive process was stopped. Runner directories were preserved and no source
or evidence directory was deleted.

M2 remains open and M3 remains blocked. If execution resumes, it must select
and validate the intended exact promotion commit, run a fresh uninterrupted
`candidate-1h`, then run `release-3h` against that exact retained candidate and
dedicated-100k capacity pair. The cancelled historical checkpoint cannot be
resumed, combined with another partial run, or treated as a waiver.
