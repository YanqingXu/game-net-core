# HP5 Owner-Local Credit Lease Prestudy

Date: 2026-08-25

Decision: `KEEP-EXPERIMENTAL`

Evidence class: development-only modeled atomics, non-promotion

## Scope And Lifecycle

The default-off prototype uses exact owner-local connection and loop pending
bytes while reserving conservative chunks from shared atomic server/global
budgets. One constructing loop owner owns fixed connection slots, generations,
pending values, retained credit and stop settlement. Parent budgets own only
atomic counters and outlive leases through shared ownership.

No callback runs. Foreign local mutation is rejected; production cross-thread
`trySend` continues to use the exact atomic path. Stop seals admission, cancels
remaining connection bytes, invalidates handles and returns all parent credit.

## Correctness Evidence

The focused Windows MSVC Release contract passed 1/1 and its MSVC Debug
AddressSanitizer run passed 1/1. It covers invalid options, fixed slot capacity,
generation reuse, foreign-owner rejection, connection/loop/server/global hard
limits, global-after-server rollback, concurrent parent no-overshoot, cached
credit reuse/trim, release validation, cancel/retire and exact stop settlement.

Four unchanged production regressions passed 4/4: TcpOutputMemoryBudget,
TcpServer hierarchical output memory, broadcast large fanout and broadcast
outstanding budget.

## Development Measurement

Ten sequential Release samples per payload used 200,000 immediate reserve/
release messages after an unrecorded warmup. The exact model mutates four
shared scopes on every admission and release; the candidate accounts connection
and loop locally and retains one 64 KiB server/global lease. All samples matched
checksums and exact accepted/released bytes and reached zero residue.

| Payload | Exact median | Lease median | Median paired improvement | Exact / lease modeled atomic mutations |
| --- | ---: | ---: | ---: | ---: |
| 512 bytes | 11,685,150 ns | 833,150 ns | 92.87% | 1,600,000 / 4 |
| 16,384 bytes | 11,681,000 ns | 833,350 ns | 92.86% | 1,600,000 / 4 |

## Missing Evidence And Decision

This is a structural single-owner model with immediate releases. Its telemetry
atomics add measurement overhead, and it does not exercise real TcpConnection
payloads, cache-line contention, multiple connection distributions, overload
recovery, idle-credit fairness/reclamation, broadcast, Linux, P99/P999 or a
clean fixed-lab HP0 ledger.

The exact accounting and large structural reduction justify
`KEEP-EXPERIMENTAL`. They do not justify production integration. Existing
TcpConnection/TcpServer/Broadcast budgets, APIs and defaults remain unchanged;
formal HP5 stays `planned`, and the one prestudy slot is released.
