# API-R1 Stable Core Independent Review

Status: `approved-for-candidate-freeze`. The first independent pass rejected
the proposed 0.3 surface with eight blocker groups. After remediation and
multiple read-only re-review passes, the independent reviewer confirmed that
every blocker is closed and returned `APPROVE`.

API-R1 approves a source surface for later candidate freeze. It does not freeze
a release SHA, promise ABI compatibility, qualify performance/endurance
evidence, or authorize publication.

## 1. Review identity and scope

```text
historical baseline tag: v0.2.0-phase4-preview
historical baseline commit: 7668d6b82a0d815ccd79f83c572bc0a36bcceea0
implementation base: 12adb00f6934e50994e755125542a1f6f2682116
reviewed state: implementation base plus the API-R1/GOV remediation worktree
reviewer: Codex independent reviewer /root/api_r1_independent_review
review date: 2026-08-05 (Asia/Shanghai)
reviewer is not the implementation author: yes
final candidate SHA: none
```

Review order followed the repository contract: intent, public contract,
invariants, thread affinity, ownership, lifecycle, implementation, and direct
tests. Gateway and deferred transport modules were excluded.

## 2. Deterministic surface inventory

The archived historical diff is
[`api-r1-public-api-diff.json`](api-r1-public-api-diff.json). It reports:

| Change relative to 0.2 Preview | Count |
| --- | ---: |
| Added stable Core headers | 10 |
| Added provisional headers | 4 |
| Changed stable-header fingerprints | 19 |
| Removed headers | 0 |
| Header category moves | 0 |
| Target additions/removals/moves | 0 |

The supported target remains only `GameNet::core`. Protocol, transport,
game-session, game-logic, broadcast, and metrics surfaces remain provisional.
Backend IOCP/epoll headers remain platform-internal. `SocketTypes.h` now owns
the supported aliases directly; the platform path is only a compatibility
wrapper. Backend association/completion hooks are private EventLoop/Poller
implementation contracts.
The separately archived same-line result is
[`api-r1-public-api-compatibility-diff.json`](api-r1-public-api-compatibility-diff.json);
it is a literal zero diff against the reviewed 0.3 surface.

## 3. Initial rejection and resolution

| Initial blocker | Resolution | Direct evidence |
| --- | --- | --- |
| Cross-thread admission could claim success after shutdown, and TCP wrappers collapsed failure reasons | EventLoop admission is linearized with executor close; timers reject a closed owner; Tcp send/auth/client-retry operations expose `Accepted`, `QueueFull`, `Shutdown`, and `OwnerUnavailable` where applicable | `test_event_loop.cpp`, `test_tcp_connection_queue_saturation.cpp`, `test_tcp_client_cross_thread_retry_config.cpp`, `test_tcp_server_contract.cpp` |
| Stable/platform classification leaked backend definitions and IOCP hooks | Stable socket aliases moved to `SocketTypes.h`; platform wrapper delegates to it; Poller/EventLoop backend hooks became private and test access uses an internal harness | public API manifest, platform backend guard, install consumer |
| Configuration thread/state rules were incomplete | Acceptor/TcpServer/TcpClient setters assert their owners; Acceptor/TcpServer reject late mutation; Connector retry delay freezes after first start; runtime connector callback/retry-enable mutation stays explicitly owner-only | Acceptor, Connector, TcpClient, and TcpServer negative contracts |
| Installed callbacks/destruction contracts were incomplete | Stable headers now state owner thread, re-entry, exception policy, raw-loop invalidation, descriptor ownership, and destruction boundaries | manifest-classified public headers and lifecycle contracts |
| Raw pointer/length preconditions were absent | Buffer, Logger, SocketsOps, and TcpConnection document valid ranges/lifetimes; Buffer/TcpConnection reject invalid null/range combinations deterministically | `test_buffer_contract.cpp`, `test_tcp_connection_queue_saturation.cpp` |
| Option validation and freeze timing were absent | Public headers define defaults, invalid ranges, owner, and pre-start/first-start freeze points; direct negative cases enforce them | option/config contract tests |
| The within-0.3 source promise was not executable | CMake package compatibility is `SameMinorVersion`; the reviewed 0.3 snapshot is a blocking zero-diff CI baseline | API manifest test, install package contract, all producer workflows |
| Install consumer mixed stable and provisional targets and omitted stable includes | Core consumer includes every stable manifest header and links only `GameNet::core`; a second executable covers provisional exports; 0.2/0.4 version probes are rejected | install consumer configure/build/CTest and static package contract |

## 4. Threading, result, and lifecycle decisions

| Surface | Owner / cross-thread rule | Observable failure or lifecycle rule |
| --- | --- | --- |
| `EventLoopExecutor::post` | callable cross-thread | `Accepted`, `QueueFull`, `Shutdown`, `OwnerUnavailable` |
| `EventLoop::tryQueueInLoop` | callable cross-thread | `false` for capacity or sealed admission; typed callers use the executor |
| timer creation | owner must still be Running | throws `logic_error` after admission closes; no valid-but-stranded `TimerId` |
| `TcpConnection::send` | owner or marshaled cross-thread | distinct scheduling `QueueFull`/`OwnerShutdown`/owner-unavailable outcomes plus connection/output-budget results |
| `TcpServer::tryMarkConnectionAuthenticated` | owner or marshaled cross-thread | typed `PostResult`; null connection is `OwnerUnavailable` |
| `TcpClient::tryEnableRetry/tryDisableRetry` | owner or marshaled cross-thread | typed `PostResult`; legacy void wrappers intentionally discard it |
| Acceptor/TcpServer configuration | base owner, before listen/start | wrong-thread assertion; late mutation `logic_error`; invalid ranges `invalid_argument` |
| Connector retry delay | connector owner, before first start | first-start freeze survives stop/restart |
| TcpClient callbacks/options | client owner before use | wrong-thread mutation rejected; callback re-entry/exception behavior documented |
| raw EventLoop pointers returned by thread/pool | borrowing only | invalid when the owning thread/pool stops or is destroyed |

Accepted descriptors transfer exactly once at the documented callback/Socket/
TcpConnection boundary. Callback-supplied buffers and address pointers are
borrowed only for the call. Owner-loop objects must be stopped/drained and
destroyed according to their installed-header contracts.

## 5. Compatibility decision

The historical comparison changes compatibility line 0.2 to 0.3, so its
machine flag correctly says that no *same-line* decision is required. API-R1
nevertheless reviewed and accepts these cross-line source breaks:

- `Connector::start()` is now owner-loop-only instead of implicitly marshaled,
  and `setRetryEnabled()` is no longer `noexcept`;
- explicit options constructors may reject copy-list initialization such as
  `T value = {}` while direct/default construction remains available;
- `Buffer::readFd` changed exact member-function type, affecting consumers that
  store that member pointer;
- direct application construction/admission of `TimerQueue` is withdrawn.
  Applications must use EventLoop's timer facade so `Accepted` always targets
  the queue actually driven by that loop. This intentionally removes the old
  `TimerQueue::addTimer/cancel` application surface and its member pointers.

These decisions do not permit drift after the 0.3 surface review. The blocking
baseline is
[`v0.3.0-api-r1-reviewed.json`](../../api/baselines/v0.3.0-api-r1-reviewed.json).
The API diff against it is zero. CI writes that result separately with
`--compatibility-output` and passes `--fail-on-compatibility-decision` plus
`--fail-on-stable-surface-review`; a later stable header/target addition,
removal, move, or fingerprint change fails.

The snapshot uses `UNBOUND-REL-C1` for its source commit because no final
candidate exists. REL-C1 must bind the final candidate SHA and re-prove the
zero diff. API-R1 does not claim ABI compatibility; all pre-1.0 consumers must
rebuild for each release/toolchain combination.

## 6. Verification record

Completed before final re-review:

- eight focused negative/typed-result contract executables: 8/8 passed;
- fresh Release install tree and consumers: stable plus provisional, 2/2
  passed;
- package version probes: 0.3.0 accepted, 0.2.99 and 0.4.0 rejected;
- public manifest, install-package, workflow, and long-soak static guards
  passed;
- historical diff regenerated with 10 stable additions, four provisional
  additions, 19 stable fingerprint changes, and no removals/moves;
- reviewed 0.3 baseline comparison: zero diff; synthetic stable drift is
  rejected, including additive stable header and target drift.

The complete Windows/IOCP Release suite passed 120/120, the fresh stable and
provisional install consumers passed 2/2, and the independent reviewer reran
the four API/package/workflow guards plus the blocking CLI gate. The final
governance sync passed all 36 repository/API/CI guards; archive equivalence
and `git diff --check` also passed.
Remote same-SHA CI and endurance remain REL-V/REL-E work after REL-C1.

## 7. Independent decision

`APPROVE` — 2026-08-05, Codex independent reviewer
`/root/api_r1_independent_review`. No implementation or contract blocker
remains. The proposed stable Core surface may proceed to REL-C1 candidate
freeze. The reviewer made no file changes.
