---
status: active
target: gamenet_hp5_credit_lease_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: native
source_commit: none
source_paths: none
---

# Use-Case Intent: HP5 Owner-Local Credit Lease Prestudy

Prestudy state: `KEEP-EXPERIMENTAL`.

## Intent

This concluded `EXPERIMENTAL-PRESTUDY` slice evaluates whether one fixed
owner-local loop lease can account
connection and loop pending bytes locally while reserving conservative byte
chunks from shared server/global hard budgets. It compares that structure with
the current exact per-message atomic connection -> loop -> server -> global
model without changing production TcpOutputMemoryBudget or TcpConnection.

## Ownership And Threading

- a shared parent budget owns only atomic reserved/peak/rejection counters and
  no EventLoop, connection, payload or callback;
- one constructing loop owner exclusively registers/retires connection slots,
  reserves/releases their bytes, refills/trims credit, stops and destroys the
  lease. Stable slot index plus generation rejects stale connection handles;
- no callback is invoked. A future accepted cross-thread public `trySend`
  remains on the existing exact production chain; only work already marshaled
  to its owner may consume local credit in this prototype;
- foreign mutation returns `WrongOwner` before observing or changing local
  slot, credit or lifecycle state.

## Capacity, Admission, And Settlement

- connection and loop actual pending bytes are exact owner-local values. A
  candidate reserve checks those hard limits before acquiring parent credit;
- refills reserve server then global atomically. Global rejection rolls back
  the server reservation before returning; no parent or local hard limit may
  be exceeded, even with concurrent independent loop leases;
- idle credit is a conservative upstream reservation, never spendable beyond
  the connection/loop limits. Explicit trim returns it; stop returns all credit;
- stopped, stale, connection-limit, loop-limit, server-limit and global-limit
  outcomes are distinct. Zero-byte reservation is accepted without credit;
- release cannot exceed the selected connection's pending bytes. Connection
  retirement requires zero pending bytes unless explicit cancel records every
  remaining byte as discarded;
- shutdown seals new registration/admission, cancels remaining connection
  bytes, returns all server/global credit and requires exact
  `acceptedBytes == releasedBytes + discardedBytes` with zero residue.

## Evidence Boundary

The benchmark counts modeled shared atomic mutations for the exact four-scope
baseline and the two-parent leased candidate, then reports elapsed time,
accepted/released bytes, checksum and residue. Local Windows timing and modeled
operation counts are development evidence only: they do not measure cache-line
traffic, real TcpConnection concurrency, fixed-lab Profile A/B, Linux, broadcast
or saturation recovery and cannot authorize integration.

## Verification

- `tests/contract/tcp_connection/test_credit_lease.cpp` covers options, owner
  affinity, generation reuse, connection/loop/server/global rejection,
  rollback, concurrent parent no-overshoot, idle-credit trim/reuse, release
  validation, cancel/retire/stop and zero residue;
- unchanged `test_tcp_output_memory_budget.cpp`, hierarchical TCP server budget
  contracts and broadcast outstanding-budget contracts remain required
  production regressions;
- `tests/cmake/test_hot_path_benchmark_contract.py` enforces the isolated target,
  one-active-prestudy rule and absence from installed/public production paths.

## Non-Goals

- no production budget/TcpConnection/TcpServer/EventLoop/Broadcast edit, public
  credit type, new send path, default, install/export, version or release;
- no relaxed admission, oversubscription, approximate terminal accounting,
  callback, socket I/O or connection migration;
- no promotion conclusion before HP0 fixed-lab evidence and a formal replay.

## Prestudy Decision

Windows MSVC Release and focused Debug AddressSanitizer contracts passed, as
did four unchanged production TCP/server/broadcast budget regressions. Ten
local Release samples at 200,000 immediate reserve/release messages preserved
checksums and exact zero-residue settlement. At both 512 and 16,384 bytes the
modeled exact hierarchy performed 1,600,000 shared atomic mutations versus four
for the retained-credit candidate; paired median elapsed improvements were
92.87% and 92.86% respectively.

The result is `KEEP-EXPERIMENTAL`, not integration. The microbenchmark has one
owner and immediate releases, instruments its own atomics, does not exercise
real TcpConnection/cache contention/fairness/idle-credit reclamation, and lacks
Linux plus HP0 fixed-lab evidence. Production budgets and admission remain
unchanged, formal HP5 remains planned, and the prestudy slot is released.
