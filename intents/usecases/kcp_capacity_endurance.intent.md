---
status: active
target: gamenet_kcp_validation
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: native
source_commit: none
source_paths: none
---

# KCP-C4: bounded capacity, fault recovery and development endurance

## Authorization and scope

The user authorizes C4 on 2026-11-17. Provide a default-OFF, non-installed,
non-exported validation executable and independently verified evidence. Existing
KCP/Core/UDP/endpoint APIs, wire and defaults remain unchanged. This is development
evidence only, never HP0, production capacity or the TCP candidate-1h/release-3h
gate. No discovery, authentication, PMTU or automatic reconnect policy is added.

## Owner, ownership and reentry

One explicit child EventLoop owns every KCP/UDP fixture, proxy, timer, snapshot
and callback. Public native UDP proxy sockets forward unchanged datagrams, with
bounded native output and no second queue. Callback payloads are copied when
retained. Existing callbacks may reenter/close; ledger commitment precedes the
next phase. The executable owns fixtures through stopped/native retirement and
releases them on their owner; handles cannot retain a replacement generation.
The supervisor owns the child process, PID/RSS observation, logs and evidence,
and never reads mutable KCP state. Signals/stop use only existing public lifecycle
capabilities. No owner waits for another loop or supervisor callback.

## Preregistered profiles and invariants

- Session counts are exactly 1, 8 and 32, on IPv4 and IPv6. Explicit fresh convs
  and known numeric peer addresses are test configuration, not authentication.
- Hub session capacity is N and protocol budget N MiB; each of N peer engines
  has capacity one and a one-MiB protocol budget. Actual arena sizes are measured,
  not assumed ABI constants. A separate one-arena budget proves protocol-memory
  rejection. Closing records remain charged until actual terminal retirement.
- Pending count/byte, datagram, proxy and per-turn work limits are explicit;
  attempts beyond the session/pending/protocol limits reject with no reservation.
- Each cycle first blackholes native traffic until pending pressure and actual
  wire drops are observed. At N>1, restore healthy peers while peer zero remains
  blocked and require their ACK/application progress. N=1 records this comparison
  as inapplicable; N=8/32 must execute it. Restore peer zero, drop an actual ACK,
  and require exact ordered empty/fragmented-message recovery.
- Force an unreachable-send terminal, close the old generation, reopen with
  fresh conv, reject stale handles/reused convs, and exchange the second
  generation. Force a local pending cancellation before teardown. Exercise
  bounded conv-history exhaustion without silently increasing limits.
- Local admission, Acknowledged/Cancelled/Failed and actual receive are distinct
  ledgers. Every accepted id terminates once; rejected sends have id zero.
  Each completed cycle has exact expected counts and zero pending bytes/messages,
  protocol storage, UDP storage, native operations and lifecycle nodes.
- The process stays alive across cycles, retains only bounded current-cycle
  ledgers, and emits flushed monotonic heartbeats plus final settlement.

## Evidence contract

The runner writes gamenet.kcp_capacity_endurance.v1, with strict child JSONL,
command/options, preregistered profile, real PID, timestamps, monotonic durations,
cycle ledgers, toolchain/build options, executable/source identities and raw
SHA-256 inventory. Source identity is read independently from Git; dirty/WSL
classification remains explicit. A missing measurement is null with a reason,
never an invented zero. Current RSS and OS process high-water observations refer
to the actual child; ready/post-cycle baselines and final samples remain distinct.
RSS is observational process memory, not a sum of owned protocol counters.

Release smoke validates all six N/family profiles. Continuous development runs
use one uninterrupted process for at least 1800 seconds on each platform,
alternating IPv4/IPv6 cycles at N=32; each family must complete. Each Release
run preregisters a 512-MiB process peak and 64-MiB post-warmup growth ceiling.
Sanitizer smoke preserves all semantic/resource contracts and records RSS as
diagnostic rather than applying Release allocator budgets. It cannot claim a
30-minute run. The supervisor enforces heartbeat/phase/overall deadlines,
retains failed/interrupted checkpoints and kills/joins a stuck child.

The independent reader accepts soak evidence only from a Release build.
Manual CI runs the Windows/IOCP and Linux/epoll Release samples at the same
dispatched commit and retains each platform's raw evidence for separate readback.

Missing/duplicate/malformed records, nonzero child exit, early duration, process
replacement, omitted fault profile/family, counter/parameter drift, nonzero
residue or missing/tampered raw evidence fails closed. Independent readback
recomputes hashes and counters; copied summaries alone cannot pass.

## Verification before implementation

- tests/ci/test_kcp_capacity_endurance.py: evidence rejection, continuous-duration,
  identity/PID, ledger, RSS, heartbeat, tampering and interrupted-run contracts.
- tests/cmake/test_kcp_validation_contract.py: default-OFF, dependency errors,
  optional executable, physical install/export absence and no production reverse
  dependency.
- benchmarks/kcp_validation/main.cpp: real native capacity/fault/cleanup cycles;
  tools/run_kcp_capacity_endurance.py supervises and independently reads evidence.
- Existing KCP five lanes, complete Clang TSan inventory/probes, 20k protocol and
  session fuzz, independent upstream peers and package consumers remain required.
  C4 smoke is added separately to the sanitizer/full-Clang gate without claiming
  skipped, filtered, shortened or restarted execution as endurance.
