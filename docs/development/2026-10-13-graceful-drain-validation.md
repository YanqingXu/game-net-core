# Accepted TCP Output Drain — 2026-10-13

The accepted-send/graceful-shutdown defect is fixed and verified on source
commit `41ba8f4ebeda784ff2952b87e53b48310d4f053f`. The same clean commit has
complete six-producer CI evidence and full local Windows/WSL development
baselines. Formal HP0 remains `DEFER` because its native fixed-lab and complete
cost-observation requirements remain unsatisfied.

This record was written after validation. Its later documentation commit is
not the source commit measured here. Local precommit checks, clean-commit
measurements, and remote CI are distinguished below.

## Contract and implementation

The original source was `bf3dab5c3f969c4bfce192dc62b2ccc1c60b3125`. With the
owner paused before dispatch, a foreign thread called `trySend(16384 bytes)`
followed by `tryShutdown()`. Both returned `Accepted`, but the lifecycle phase
could half-close before the normal send task; both IOCP and epoll delivered
zero bytes followed by EOF. Pending-byte convergence did not prove delivery.

Fix commit `cb79b4bbbdf48fdc2ec863437f5a48a209dcaa35` makes send reservation
and the permanent shutdown seal share one atomic modification order. A
reservation acquired before the seal must settle before graceful half-close;
later reservations are rejected. Public byte snapshots mask the internal seal.
Only the first shutdown seal creates the owner-side graceful-close obligation,
which is retired after the write half-close. The final reservation release or
rollback signals the lifecycle source without requiring normal queue admission.

The relevant intent is
[`tcp_connection.intent.md`](../../intents/modules/tcp_connection.intent.md).
The corresponding thread-affinity, ownership and testing rules were updated
before implementation. The contract answers are:

| Boundary | Verified behavior |
| --- | --- |
| Owner | The connection's EventLoop thread owns Channel/socket mutation, buffering, callbacks and half-close. |
| Lifetime | Send closures retain the connection while accepted work is queued; the existing lifecycle node uses its weak connection binding. No new reference cycle or releaser is introduced. |
| Re-entry | Existing callbacks may request send/shutdown/force-close. The permanent seal rejects new sends and repeated shutdown cannot recreate a completed half-close obligation. |
| Cross-thread | Send admission and shutdown seal are atomic; accepted payload work is marshaled through the owner queue and close progress through the lifecycle source. Foreign threads do not manipulate the socket or Channel. |
| Direct contract | `tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp` |
| Public API | No installed declarations or targets changed; the reviewed v0.3 public API comparison reports zero difference. The header change documents behavior. |

The direct contract covers a paused owner, multiple functor turns, large/partial
output, directly completed writes, dropped optional completion notifications,
producer allocation failure and normal-post rejection after shutdown, and
concurrent send/shutdown with a running owner. It verifies payload before EOF,
final pending bytes, and repeated shutdown after completion. The Windows
Release contract also passed 50 consecutive executions.

The initial Windows Debug/ASan fault-injection fixture incorrectly matched a
small STL debug proxy allocation inside a `noexcept` constructor. The final
fixture filters by payload allocation size; the isolated allocation experiment
and the failed/superseded logs remain in `out/project-review/`. No sanitizer
was disabled and the allocation-failure rollback case remains covered.

## Exact-commit remote CI

[CI run 38336714098, attempt 1](https://github.com/YanqingXu/game-net-core/actions/runs/38336714098)
completed successfully on the exact `41ba8f4` source commit. All six producers
and their aggregate succeeded, using hosted Ubuntu 24.04 and Windows runners.
The runner-routing fix makes the repository's hosted setting effective on both
platforms; explicitly selected self-hosted runners remain supported and fork
PRs continue to use hosted runners.

| Producer | Executed tests | Additional evidence |
| --- | --- | --- |
| Linux CMake Debug | 130/130 | Installed consumer 2/2; experimental opt-in consumer 1/1 |
| Linux Release | 130/130 | Release guards, benchmark and capacity checks |
| Linux ASan/UBSan | 130/130 | Packet-framer fuzz smoke: 1,000 executed units |
| Linux TSan | 103/103 | Registered threading lane; this is not the full 130-test suite |
| Windows MSVC Debug | 130/130 | Installed consumer 2/2 |
| Windows MSVC Release | 130/130 | Installed consumer 2/2 and release consumer checks |

All seven complete artifact ZIPs were downloaded and their SHA-256 digests
matched GitHub's reported digests. Running
`tools/verify_ci_evidence_set.py` independently against the six producer
directories returned zero. The rebuilt aggregate matched the remote aggregate
except for its generation timestamp. Producer manifests bind the same source
SHA, run ID and attempt. The durable compact identities are recorded in
[`ci-verification.json`](benchmark_results/2026-10-13-local-hp0-41ba8f4/ci-verification.json);
full archives and unpacked evidence remain in `out/project-review/ci-41ba8f4/`.

## Local correctness and continuous prestudies

The precommit source checks passed Windows Release 130/130 and WSL Ubuntu
24.04/GCC 13.3 Release 130/130, Windows focused ASan 10/10, and Ubuntu 24.04
focused ASan/UBSan 7/7 and TSan 7/7. These are local same-source checks, not
substitutes for the exact-commit CI above. The 46-script engineering guard
pass includes the updated platform-document contract; an earlier 45/46 log
retains its obsolete hosted-runner text assertion as superseded evidence.

`.github/workflows/prestudy-contracts.yml` adds a separate weekly/manual matrix:
Linux Release, ASan/UBSan and TSan, plus Windows Release and ASan. Its runner
builds exactly the seven isolated HP1–HP6 contracts and verifies inventory and
JUnit results, rejecting omitted, disabled, skipped or failed contracts. Output
is explicitly `development-contracts`, with `promotion_eligible=false`.

The new workflow cannot be dispatched before it reaches the default branch:
GitHub returned HTTP 404 for the branch dispatch on 2026-10-13. This is not a
completed remote prestudy run. The same runner was instead exercised locally
against clean `41ba8f4` checkouts: Windows Release and ASan, and WSL Ubuntu
24.04 Release, ASan/UBSan and TSan each passed exactly 7/7 with no skips. Final
manifests and JUnit hashes are recorded in the accompanying evidence index.
Earlier Ubuntu 22.04 prestudy sanitizer
checks are retained separately and are not relabeled as Ubuntu 24.04.

## Full local HP0 development baseline

The user-selected machines were native Windows 10/IOCP and WSL Ubuntu
24.04.4/epoll on an Intel Core i5-14600K. Both ran Release builds from the same
clean `41ba8f4` checkout. Linux sampling finished before Windows sampling began.
Git identity and executable hashes were checked before and after collection.

Each original scenario ran one excluded warmup and ten scenario-round-robin
recorded samples. The unchanged inventory includes 250,000 framing messages,
4,096 broadcast fanout and 10,000 connections. Linux's child-only soft
`RLIMIT_NOFILE` was raised from 10,240 to 65,536 within its existing hard limit
of 1,048,576; the workload was not reduced. Each platform retains 66 actual
command/stdout/stderr execution sets and 60 canonical sample JSON files:
six warmups plus sixty recorded executions, 132 child executions in total.

Both ledgers independently passed `tools/validate_hot_path_cost.py`. A second
summary pass checked raw stdout against canonical samples, inventory parameters,
warmup semantics and raw-file hashes, and recomputed all primary/guardrail
statistics. The following are medians, excluding warmups; full ranges and
guardrails are in [`summary.json`](benchmark_results/2026-10-13-local-hp0-41ba8f4/summary.json).

| Scenario / primary unit | Windows IOCP | WSL Ubuntu 24.04 epoll |
| --- | ---: | ---: |
| Framing, operations/s | 7,140,205 | 36,202,977 |
| Network-to-logic, messages/s | 353,720 | 5,567 |
| TCP ping-pong, messages/s | 284,956 | 277,998 |
| Broadcast, operations/s | 3,731,924 | 4,391,123 |
| 10k connection establishment, connections/s | 17,145 | 8,028 |
| Overload recovery, ms | 272.214 | 273.889 |

These are separate environment baselines, not a paired baseline/candidate
experiment or a ranking of operating systems/backends. The network-to-logic
gap and Windows fanout-tail variation are diagnostic leads, not an attributed
code regression or promotion decision. All overload teardown guards were true.

[`raw-evidence.zip`](benchmark_results/2026-10-13-local-hp0-41ba8f4/raw-evidence.zip)
retains the two ledgers, their inventory/manifest-indexed raw files, diagnostic
sidecars, launch identity and summary replay script. The archive's hashes and
validation outcomes are in
[`evidence-index.json`](benchmark_results/2026-10-13-local-hp0-41ba8f4/evidence-index.json).
After extraction, either ledger can be checked with:

```text
python tools/validate_hot_path_cost.py --input EXTRACTED/windows/hot-path-cost.json
python tools/validate_hot_path_cost.py --input EXTRACTED/linux/hot-path-cost.json
```

The raw archive preserves original byte content and recorded execution paths;
it does not claim that absolute binary paths will exist on another machine.
Diagnostic sidecars do not replace the ledger's indexed hashes or validator.

## Remaining HP0 conditions

Both ledgers are `development` and `promotion_eligible=false`. Each lacks 380
required sample-cost observations; these entries remain `null` with reasons,
not estimated zeros. No complete reviewed observer was configured. The WSL
probe found no CPU PMU and `perf_event_open` returned `ENOENT` for cycles,
instructions and LLC misses. WSL is not native Linux evidence. Windows inherited
logical CPU mask `0xfffff` (0–19) with an unmanaged Balanced frequency policy;
WSL exposed CPUs 0–11 with unmanaged frequency. Neither is a fixed lab policy.
The repository reported zero registered self-hosted runners.

The remaining action is to provision the native Linux/Windows fixed performance
environment and a reviewed complete PMC/ETW/perf plus allocation/copy/syscall/
wakeup observer, pass preflight, then collect and independently validate both
same-commit ledgers with `--require-fixed-lab`. The existing full-load gate was
not weakened. HP1–HP8 integration, default-path switches and release promotion
remain behind formal HP0 and their original 5%/3% criteria.
