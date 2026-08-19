# Slow and Mixed Broadcast Recovery Capacity Profiles

`gamenet_capacity_profile` is the M3-Q1 bridge into the M3-P1 capacity
matrix. Its `slow-broadcast-recovery` and `mixed-pressure-recovery` scenarios
combine real TCP slow readers, `TcpTransportEndpoint`,
`BroadcastRouter`/`BroadcastDispatcher`, hierarchical TCP output budgets,
retained Buffer capacity, fixed network storage, and process RSS in one run.
The mixed scenario additionally measures short-lived healthy echo traffic
while the slow-reader pressure and recovery phases are active.

It is opt-in benchmark tooling, not CTest and not an installed target. The
slow-only stdout document remains `gamenet.capacity_profile.v1`. Historical
mixed evidence uses `gamenet.capacity_profile.v2`; the scale-ready bounded
recovery-reader contract uses `gamenet.capacity_profile.v3`.

## Contract

One run has three phases:

1. Connect the configured number of real TCP clients, set each receive buffer
   to 4 KiB, request the configured finite server send buffer from each
   connection's owner-loop established callback, and do not read.
2. Route and dispatch all shared payloads. Wait for every endpoint to publish
   one terminal result, then sample pending, retained, fixed-storage, and RSS
   peaks while the readers remain stalled.
3. Start every reader. Recovery is reported only after aggregate connection
   pending bytes are at or below the configured threshold and Broadcast
   outstanding tasks/bytes are both zero for the complete stable window.

The producer and `tools/validate_capacity_profile.py` require:

- aggregate connection pending peak does not exceed
  `connections * connection_hard_limit_bytes`;
- Broadcast outstanding peak does not exceed its global limit;
- accepted plus dropped endpoints equals `connections * messages`;
- client-received bytes equal accepted endpoints times payload bytes;
- every drop is typed, and `EndpointOverloaded` equals the sum of connection,
  loop, server, and optional global TCP rejection counters;
- recovered Buffer retained capacity is within the per-Buffer target,
  connection-local read storage is within the platform per-connection limit,
  and process fixed storage is zero after full teardown.

RSS before/pressure/recovery/peak/after is observational. Allocator and kernel
retention vary by platform, so this schema deliberately defines no RSS
performance threshold. The reported peak is nevertheless structurally
coherent: it is the maximum of the independent 1 ms sampler and every explicit
baseline, pressure, recovery, and post-teardown sample. A short phase sample
therefore cannot exceed the reported peak merely because the sampler missed it.

`TcpConnection::memoryRetentionSnapshot()` is owner-loop-only. The profile
groups connections by executor identity and posts one low-frequency batch per
owner loop; that callback samples all of its local connections. It never reads
connection-owned Buffer state directly from its driver thread, and the sample
does not inject one normal-queue task per connection while healthy probes are
running.
`TcpConnection::setSendBufferSize()` is also owner-loop-only. A zero profile
value preserves the operating-system default; a positive value is a requested
native `SO_SNDBUF` size and the operating system may round the effective size.
Historical documents that predate this parameter are interpreted as the zero
default, while the fixed candidate gate requires the field and exact value.
The fixed candidate/dedicated value is an evidence parameter, not a replacement
for the application-level pending-output hard limit.

## Mixed pressure contract

`mixed-pressure-recovery` preserves the v1 slow-reader and Broadcast contract.
It starts one persistent bounded probe worker pool immediately after all slow
targets and dispatch progress have been published. The pool paces an exact
number of attempts over one monotonic interval. Each attempt has a nonblocking
connect deadline, sends one fixed small payload, requires its exact echo, and
closes abortively. Post-publication probe connections are classified under the
benchmark coordination mutex and never become Broadcast targets.

At most one probe batch is live. Its persistent workers first establish every
client socket in the batch and keep successful connects open until the server's
cumulative accepted count converges. Only then do they send the payload,
require the exact echo, and close abortively; the next batch is not released
until the cumulative closed count also converges. This ordering keeps the
existing client I/O deadline strict without letting it close a TCP-established
probe before the owner-loop connection callback can publish that accept.
Loop/server output budgets reserve headroom for that one batch; the
per-connection slow client limit and Broadcast routing/dispatch limits do not
change.

After the pressure sample, v3 hands each slow socket to exactly one member of a
fixed-size recovery-reader pool. Each worker owns a stable disjoint shard,
performs bounded nonblocking reads, and remains alive through graceful server
close. The driver does not touch those sockets until all reader workers join.
The v3 artifact records the configured concurrency ceiling and exact
worker/assigned/closed counts.

The mixed validator requires:

- `attempted = probe_succeeded + typed failures`;
- client connected = server accepted = server closed;
- client connected = successful probes plus post-connect typed failures;
- exact attempt and batch counts from the configured rate, duration, and batch
  size;
- zero connect, send, receive, and payload-mismatch failures;
- a complete paced interval, a recomputable actual attempt rate, and bounded
  connect/probe/schedule-lag nearest-rank P99 values.
- for v3, actual reader workers equal
  `min(reader_concurrency_limit, connections)`, every slow socket is assigned
  exactly once, and every assigned socket reaches a terminal client-side close.

## Build and run

```powershell
cmake -S . -B build-capacity -DGAMENET_BUILD_TESTING=OFF -DGAMENET_BUILD_BENCHMARKS=ON
cmake --build build-capacity --config Release --target gamenet_capacity_profile

build-capacity\benchmarks\Release\gamenet_capacity_profile.exe `
  --scenario slow-broadcast-recovery `
  --connections 4 --threads 2 --messages 64 `
  --payload-bytes 262144 `
  --low-water-bytes 262144 `
  --high-water-bytes 524288 `
  --hard-limit-bytes 2097152 `
  --recovery-threshold-bytes 0 `
  --pressure-settle-ms 500 `
  --recovery-stable-ms 250 `
  --timeout-ms 30000 `
  --iocp-accept-depth 8 |
  py -3 tools\validate_capacity_profile.py `
    --expected-platform windows `
    --expected-backend iocp `
    --expected-build-type Release `
    --expected-connections 4 -
```

Linux uses the same target and arguments, with the executable under
`build-capacity/benchmarks/` and expected backend `epoll`.

The mixed profile extends the same scale parameters with the probe workload:

```powershell
build-capacity\benchmarks\Release\gamenet_capacity_profile.exe `
  --scenario mixed-pressure-recovery `
  --connections 100 --threads 4 --messages 10 `
  --payload-bytes 32768 `
  --low-water-bytes 32768 `
  --high-water-bytes 65536 `
  --hard-limit-bytes 262144 `
  --server-send-buffer-bytes 4096 `
  --recovery-threshold-bytes 0 `
  --pressure-settle-ms 500 `
  --recovery-stable-ms 250 `
  --timeout-ms 60000 `
  --iocp-accept-depth 32 `
  --probe-rate 100 `
  --probe-duration-ms 2000 `
  --probe-batch-size 10 `
  --probe-concurrency 4 `
  --probe-payload-bytes 32 `
  --probe-connect-timeout-ms 1000 `
  --reader-concurrency 16 |
  py -3 tools\validate_capacity_profile.py `
    --expected-platform windows `
    --expected-backend iocp `
    --expected-build-type Release `
    --expected-connections 100 -
```

## 2026-07-30 local M3-P1 seed

The first Windows MSVC Release run used the original slow-only command above
with the operating-system send-buffer default. It is a local capacity-matrix
seed, not candidate/release evidence and not a regression threshold.

| Observation | Result | Bound |
| --- | ---: | ---: |
| endpoint attempts | 256 | 4 connections × 64 messages |
| accepted / `EndpointOverloaded` | 40 / 216 | accepted + dropped = 256 |
| TCP rejection scopes | connection 216; loop/server/global 0 | total = 216 |
| aggregate pending peak | 8,388,608 B | 8,388,608 B |
| Broadcast outstanding peak | 15,728,640 B | 67,108,864 B |
| recovery | 276.303 ms | pending 0 B for a 250 ms stable window |
| Buffer retained after recovery | 8,256 B | 524,352 B across 8 Buffers |
| connection-local read storage | 16,384 B | 4 × 4,096 B |
| all fixed storage after teardown | 0 B | 0 B |
| client bytes received | 10,485,760 B | 40 accepted × 262,144 B |
| RSS before / peak / recovery | 5,029,888 / 15,380,480 / 6,578,176 B | observational |

The planned M3-P1 `Slow reader` and `Broadcast TCP` rows reuse this schema at
100/1k and 1k/10k+ scale rather than inventing a second result format.

## 2026-07-30 M3-P1-D scale seed

The scale profile reduces payload and per-connection watermarks together so
the 100- and 1,000-client runs test the same eight-payload pending bound without
manufacturing a multi-gigabyte process:

```powershell
foreach ($connections in 100, 1000) {
  build-capacity\benchmarks\Release\gamenet_capacity_profile.exe `
    --scenario slow-broadcast-recovery `
    --connections $connections --threads 4 --messages 10 `
    --payload-bytes 32768 `
    --low-water-bytes 32768 `
    --high-water-bytes 65536 `
    --hard-limit-bytes 262144 `
    --recovery-threshold-bytes 0 `
    --pressure-settle-ms 500 `
    --recovery-stable-ms 250 `
    --timeout-ms 120000 `
    --iocp-accept-depth 32
}
```

Both Windows MSVC Release artifacts passed the strict v1 validator:

| Slow clients / endpoint attempts | Accepted / overload | Pending peak | Recovery | RSS peak |
| ---: | ---: | ---: | ---: | ---: |
| 100 / 1,000 | 900 / 100 | 26,214,400 B | 267.014 ms | 30,298,112 B |
| 1,000 / 10,000 | 8,252 / 1,748 | 262,144,000 B | 331.686 ms | 274,972,672 B |

These are local capacity seeds, not portable thresholds. The 1,000-client run
also proves the endpoint-attempt accounting and recovery contract at the
planned 10k Broadcast TCP scale.

## 2026-07-30 M3-P1-D2 mixed seed

Five Windows MSVC Release repetitions used the mixed command above before the
bounded-reader scale step. All five historical v2 artifacts passed the strict
validator, as did a separate v1 compatibility run:

| Observation | Five-run result |
| --- | ---: |
| healthy attempted / succeeded / server accepted / server closed | 200 / 200 / 200 / 200 every run |
| typed healthy failures | 0 every run |
| actual attempts/s | 99.358 median; 98.898–99.423 range |
| worst connect / echo / schedule-lag P99 | 10,740.4 / 508.7 / 22,992.3 us |
| slow pending peak | 26,214,400 B every run |
| slow Broadcast accepted / overload | 900–950 / 50–100 |
| slow recovery | 262.080–283.776 ms |
| RSS peak | 30,298,112–31,965,184 B |

This is a local functional/capacity seed, not a cross-platform latency budget.
It proves that bounded healthy traffic can complete with exact lifecycle
accounting while the same server is applying and recovering from slow-reader
Broadcast pressure. The next P1-D slice promotes the matching profile into the
planned 10k candidate gate and defines the separate 100k/endurance lane.

## M3-P1-D3 candidate and dedicated gates

The manual-only `.github/workflows/capacity-gate.yml` workflow promotes the
scale-ready v3 profile without turning developer pushes into uncontrolled
capacity tests:

| Profile | Slow sockets | Broadcast endpoint attempts | Healthy probes | Recovery readers | Repetitions | Runner class |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `candidate-10k` | 1,000 | 10,000 | 500 | 16 | 3 | hosted Linux and Windows |
| `dedicated-100k` | 10,000 | 100,000 | 10,000 | 64 | 1 | dedicated Linux and Windows |

Both profiles keep the reviewed 32 KiB payload, 32/64/256 KiB watermarks, and a
4 KiB server `SO_SNDBUF` request. The finite request makes the typed application
overload phase reproducible on both Linux/epoll and Windows/IOCP without
increasing logical traffic or changing the eight-payload application hard
limit. They retain the same recovery, healthy-probe lifecycle, fixed-storage,
and RSS contracts. The dedicated profile therefore admits an aggregate pending
ceiling of 2,621,440,000 bytes and requires both provisioned runner labels and
the explicit `RUN_DEDICATED_100K` acknowledgement.

`tools/run_capacity_gate.py` owns the fixed profile definitions. It rejects
parameter drift, validates every raw v3 sample, hashes the executable,
toolchain record, and samples, then writes a canonical
`gamenet.capacity_gate.v1` manifest. The aggregation-only verifier accepts one
Linux/epoll and one Windows/IOCP artifact, revalidates their contents and
hashes, and emits `gamenet.capacity_gate_pair.v1`. Cross-host latency,
throughput, and RSS values remain observational; the pair gate compares
contract identity and pass/fail evidence rather than treating unlike hosts as
a performance contest.

If the executable returns nonzero or emits invalid JSON, the runner retains its
raw stdout as `sample-N-failure.json`. Structured failures also report the
document's `error` plus false checks in the workflow log. This keeps a failed
hosted-runner invariant diagnosable; the retained failure remains negative
evidence and never produces a passing gate manifest.

The executable explicitly flushes and checks stdout after the closing JSON
delimiter and before returning success. This prevents an otherwise successful
process from publishing only the first buffered prefix of the document.

A local Windows Release orchestration preflight completed all three
`candidate-10k` repetitions with exact 10,000 endpoint attempts, 500/500
healthy probes, and 16 workers accounting for all 1,000 recovery sockets in
each repetition. This is implementation evidence, not an immutable
cross-platform release artifact.

The 2026-08-17 PERF-R1 remediation preflight repeated the complete
`candidate-10k` profile three times on each local platform with the fixed 4 KiB
server send-buffer request. Windows produced 8,252 accepted plus 1,748 typed
overload results in every repetition; WSL Linux produced 8,000 plus 2,000.
Every sample completed 500/500 healthy probes, accounted all 1,000 recovery
sockets, converged pending output to zero, and passed the v3 validator. These
precommit results establish profile feasibility only; release evidence must be
regenerated from the immutable candidate tag on the workflow runners.

The mixed capacity gate complements rather than replaces the one-process
long-soak lane. Candidate promotion normally requires the capacity pair and
the 24-hour endurance artifact from the same frozen commit. An explicit
owner-approved `candidate-waiver` may omit the 24-hour artifact while retaining
exact paired 10k validation; `release-waiver` may omit both 24/72-hour artifacts
while retaining the exact dedicated 100k pair. Both carry visible `waived`
status instead of a duration pass. The long-soak workflow consumes exact capacity
and endurance run/attempt identities and uses
`tools/verify_production_promotion_evidence.py` to revalidate their raw inputs
before emitting the candidate/release promotion manifest.
