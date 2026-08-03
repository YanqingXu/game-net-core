# M3-R1 Closure Review Packet

Status: local Linux/epoll pre-review evidence complete; awaiting a committed
candidate SHA and independent reviewer sign-off.

This packet closes only M3-R1 / P1-01: worker establishment queue rejection
must never release an unestablished `TcpConnection` outside its selected owner
loop. It does not approve the wider 0.3 stable API or release candidate.

## 1. Evidence binding

The reviewer must record one committed candidate SHA. A dirty worktree, a
different SHA, or results copied from an earlier run cannot close M3-R1.

```text
candidate SHA:
reviewer name:
review date:
reviewer is not the implementation author: yes / no
Linux distribution and version:
compiler and version:
CMake version:
```

## 2. Required review matrix

| Requirement | Authoritative evidence | Reviewer result |
| --- | --- | --- |
| Intent defines accepted-fd ownership, provisional bookkeeping, queue admission, and owner establishment | `intents/modules/tcp_server.intent.md` | pending |
| Thread-affinity rules keep construction, rollback, `connectDestroyed`, and final release on the selected owner | `rules/thread_affinity_rules.md` | pending |
| Ownership rules provide exact-once fd/map/load/admission/deadline release | `rules/ownership_rules.md` | pending |
| Rollback storage is bounded by current worker normal-functor capacity | `TcpServer.cc`, `EventLoopLifecycleRegistry.h` | pending |
| QueueFull/Shutdown/OwnerUnavailable cannot make the base loop the final connection owner | `TcpServer::newConnection`, `resolveEstablishmentRollback`, `drainEstablishmentRollbacks` | pending |
| Accepted counter/metric is published only after normal-queue admission succeeds | `TcpServer::newConnection` and lifecycle static guard | pending |
| Stop/join waits for armed establishment rollback obligations | `TcpServer::driveWorkerStopParticipant` | pending |
| Off-owner release of an unestablished connection terminates deterministically | `TcpConnection::~TcpConnection` | pending |
| Saturation contract covers small normal+reserve capacity, two-worker selector load, per-peer admission, deadline, no callback/metric, fd close, recovery, and stop | `test_tcp_server_establishment_saturation.cpp` | pending |
| No installed public API changed | public API manifest comparison | pending |
| Linux/epoll focused 50/50 and full Debug/Release suites pass | commands in section 4 | pending |

Every row must be `pass`. `N/A`, indirect historical evidence, or an unchecked
assumption leaves M3-R1 open.

## 3. State-machine audit

The reviewer should trace these transitions and confirm that each owner is
unique and each rollback is exact once:

```text
BaseOwnsFd
  -> RollbackArmed
  -> ConnectionConstructed
  -> ProvisionalMapLoadAdmissionDeadline
  -> NormalQueueAccepted -> RollbackDisarmed -> OwnerEstablished
  -> NormalQueueRejected -> BaseScopesReleased -> OwnerDestroyed
```

Required failure decisions:

- rollback record allocation/registration failure: the base `Socket` guard
  remains the only fd owner;
- connection construction failure: the base guard closes the fd;
- provisional bookkeeping failure: committed base scopes roll back and the
  pre-armed worker obligation performs final connection release;
- QueueFull, Shutdown, or OwnerUnavailable after construction: no Accepted
  counter, metric, or connection callback is published;
- stop racing an `Arming` record: lifecycle final drain repeats until the base
  resolves the record, and worker acknowledgement cannot overtake it.

The independent review must reject any implementation that treats
“not established yet” as permission for off-owner destruction.

## 4. Linux/epoll verification

Run from a clean checkout of the candidate SHA:

```bash
set -euo pipefail
test -z "$(git status --porcelain)"

python3 tests/scope/test_intent_consistency.py
python3 tests/cmake/test_event_loop_contracts.py
python3 tests/cmake/test_tcp_lifecycle_contracts.py
python3 tests/cmake/test_threading_gate_contracts.py
python3 tests/api/test_public_api_manifest.py
python3 tools/compare_public_api_manifest.py \
  --output m3-r1-public-api-diff.json

for build_type in Debug Release; do
  build_dir="build-m3-r1-${build_type,,}"
  cmake -S . -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DGAMENET_BUILD_TESTING=ON \
    -DGAMENET_ENABLE_TLS=OFF \
    -DGAMENET_ENABLE_EXPERIMENTAL=OFF
  python3 tools/verify_ctest_inventory.py \
    --test-dir "${build_dir}" \
    --expected-total 120 \
    --expect-label threading=93 \
    --expect-label lifecycle=98 \
    --expect-label game_pipeline=7 \
    --expect-label broadcast=5 \
    --output "${build_dir}/m3-r1-ctest-inventory.json"
  cmake --build "${build_dir}" --parallel
  ctest --test-dir "${build_dir}" \
    -R '^contract\.tcp_server\.test_tcp_server_establishment_saturation$' \
    --repeat until-fail:50 \
    --output-on-failure \
    --timeout 60
  ctest --test-dir "${build_dir}" \
    --output-on-failure \
    --timeout 60
done
```

Attach the command log, both inventory JSON files, the public API diff, and the
candidate SHA to the review record. The focused run must execute through epoll;
a Windows or mocked backend result is not Linux evidence.

### 4.1 Local author pre-review run (not closure evidence)

The implementation worktree was exercised locally on 2026-08-03 before
candidate freeze:

- host: Windows 10 Pro 19045 with WSL2;
- guest: Ubuntu 24.04.4 LTS, kernel
  `6.18.33.2-microsoft-standard-WSL2`;
- toolchain: G++ 13.3.0, CMake 3.28.3, Ninja 1.11.1, Python 3.12.3;
- source state: dirty worktree based on
  `7a56132d6ea60346ec06c108cd627b7b4cd5a04f`;
- Debug: inventory 120 (`threading=93`, `lifecycle=98`), focused saturation
  contract 50/50, full suite 120/120;
- Release: inventory 120 (`threading=93`, `lifecycle=98`), focused saturation
  contract 50/50, full suite 120/120;
- the Linux builds compiled `SocketsOps_linux.cc`, `Wakeup_linux.cc`, and
  `EPollPoller.cc`, so the focused result used the native Linux/epoll path;
- the post-portability-change Windows MSVC Debug and Release suites both
  passed 120/120 through the IOCP path;
- all 36 repository/API/CI guards passed, including a public API manifest
  comparison with no M3-R1 public-surface change.

The first Linux compile also exposed two source-private test portability gaps:
the IOCP association harness leaked Windows-only types through method
signatures, and the EventLoop fair-budget fixture had only a Windows socket-pair
constructor. The worktree now guards the complete IOCP-only signatures and uses
native nonblocking `socketpair` on Linux; static contract checks protect both
paths.

These results establish local feasibility, but they deliberately do not fill in
the reviewer matrix or sign-off. Section 4 must be rerun from the clean,
committed candidate SHA before M3-R1 can close.

## 5. Independent sign-off

```text
candidate SHA reviewed:
all matrix rows pass: yes / no
Linux Debug focused 50/50 and full 120/120: pass / fail
Linux Release focused 50/50 and full 120/120: pass / fail
owner-loop destruction rule preserved without exception: yes / no
blocking comments:
decision: approve / request changes
reviewer signature or review URL:
```

M3-R1 can move from `locally-verified` to `closed` only after this section is
completed by an independent reviewer and its candidate SHA matches all Linux
evidence.
