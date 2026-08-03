# M3-R1 Closure Review Packet

Status: `request changes`. Independent review rejected candidate
`446f86d10c8c78725bf59bbabdebd7f3d1968af3` because its construction-failure
path did not preserve exact-once accepted-fd ownership. The remediation is
locally verified but requires a new committed candidate SHA and a fresh clean
independent review before M3-R1 can close.

This packet is scoped only to M3-R1 / P1-01: worker establishment queue rejection
must never release an unestablished `TcpConnection` outside its selected owner
loop. It does not approve the wider 0.3 stable API or release candidate.

## 1. Evidence binding

The reviewer must record one committed candidate SHA. A dirty worktree, a
different SHA, or results copied from an earlier run cannot close M3-R1.

```text
candidate SHA: 446f86d10c8c78725bf59bbabdebd7f3d1968af3
reviewer name: Codex independent reviewer /root/m3_r1_independent_review
review date: 2026-08-03 (Asia/Shanghai)
reviewer is not the implementation author: yes
Linux distribution and version: WSL2 Ubuntu 24.04.4 LTS, kernel 6.18.33.2-microsoft-standard-WSL2
compiler and version: G++ 13.3.0
CMake version: 3.28.3
```

## 2. Required review matrix

| Requirement | Authoritative evidence | Reviewer result |
| --- | --- | --- |
| Intent defines accepted-fd ownership, provisional bookkeeping, queue admission, and owner establishment | `intents/modules/tcp_server.intent.md` | pass |
| Thread-affinity rules keep construction, rollback, `connectDestroyed`, and final release on the selected owner | `rules/thread_affinity_rules.md` | pass |
| Ownership rules provide exact-once fd/map/load/admission/deadline release | `rules/ownership_rules.md` | **fail: candidate briefly gives the same fd to both `pendingSocket` and `TcpConnection::socket_`; constructor unwind can close it twice** |
| Rollback storage is bounded by current worker normal-functor capacity | `TcpServer.cc`, `EventLoopLifecycleRegistry.h` | pass |
| QueueFull/Shutdown/OwnerUnavailable cannot make the base loop the final connection owner | `TcpServer::newConnection`, `resolveEstablishmentRollback`, `drainEstablishmentRollbacks` | pass |
| Accepted counter/metric is published only after normal-queue admission succeeds | `TcpServer::newConnection` and lifecycle static guard | pass |
| Stop/join waits for armed establishment rollback obligations | `TcpServer::driveWorkerStopParticipant` | pass |
| Off-owner release of an unestablished connection terminates deterministically | `TcpConnection::~TcpConnection` | pass |
| Saturation contract covers small normal+reserve capacity, two-worker selector load, per-peer admission, deadline, no callback/metric, fd close, recovery, and stop | `test_tcp_server_establishment_saturation.cpp` | pass for queue rejection; missing construction-failure injection in this candidate |
| No installed public API changed | public API manifest comparison | pass for M3-R1 baseline `7a56132d` to candidate; the separate 0.2-to-0.3 diff still requires API-R1 |
| Linux/epoll focused 50/50 and full Debug/Release suites pass | commands in section 4 | pass |

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
“not established yet” as permission for off-owner destruction. It must also
reject any constructor transition in which the base guard and a partially
constructed connection can both close the same numeric fd.

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

### 4.2 Independent run for rejected candidate

The independent Codex reviewer ran section 4 from a clean checkout of
`446f86d10c8c78725bf59bbabdebd7f3d1968af3` in the isolated evidence directory
`/home/xyq/m3-r1-candidate-446f86d/reviewer-codex-20260803-Jt0glt`:

- all five section-4 static guards plus the public API diff command passed;
- Debug and Release inventories were each 120 tests with
  `threading=93`, `lifecycle=98`, `game_pipeline=7`, and `broadcast=5`;
- Debug and Release focused runs each passed 50/50;
- Debug and Release full suites each passed 120/120;
- compile commands contained `SocketsOps_linux.cc`, `Wakeup_linux.cc`, and
  `EPollPoller.cc`;
- source status was clean before and after the run.

The green runtime evidence does not override the failed ownership audit.
GitHub `ci` run `30796005701`, attempt 3, also bound all six successful producer
jobs to the same SHA. Its best-effort aggregate found 0/6 retained artifacts,
so no aggregate artifact evidence is claimed.

### 4.3 Remediation worktree checkpoint

After the rejected review, the worktree moved every fallible TcpConnection
constructor step before the connection Socket claims the fd, stored the
pre-armed rollback owner and released the base guard under the participant
mutex, and added a deterministic late-construction-failure assertion to
`test_tcp_server_establishment_saturation.cpp`.

The uncommitted remediation worktree passes on 2026-08-03:

- Windows/IOCP Debug and Release focused 50/50 and full 120/120;
- WSL2 Linux/epoll Debug and Release focused 50/50 and full 120/120;
- all 36 repository/API/CI guards.

These are author-side pre-freeze results only. They do not change the rejected
candidate decision and cannot fill the new-candidate sign-off.

The independent reviewer then performed a read-only remediation pre-review and
returned `approve-for-candidate-freeze` with no implementation blocker. That
review covered constructor ordering, the participant-mutex handoff, TcpClient's
matching pending-socket path, the failure harness, deadlock/leak risks, and the
absence of installed API changes. It explicitly did not approve closure: the
untracked source-private harness must be included in the candidate, and the
new clean committed SHA must receive the full section-4 rerun and 11-row
sign-off.

## 5. Independent sign-off

```text
candidate SHA reviewed: 446f86d10c8c78725bf59bbabdebd7f3d1968af3
all matrix rows pass: no
Linux Debug focused 50/50 and full 120/120: pass
Linux Release focused 50/50 and full 120/120: pass
owner-loop destruction rule preserved without exception: yes for fully constructed objects; construction-failure fd ownership failed
blocking comments: TcpConnection partial construction can close the fd while the base pendingSocket still owns the same numeric value
decision: request changes
reviewer signature or review URL: Codex independent reviewer /root/m3_r1_independent_review
```

M3-R1 remains open. It can move from `locally-verified` to `closed` only after
the remediation is committed as a new clean candidate and an independent
reviewer completes a new all-pass matrix against that exact SHA.
