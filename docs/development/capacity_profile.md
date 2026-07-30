# Slow Broadcast Recovery Capacity Profile

`gamenet_capacity_profile` is the M3-Q1 bridge into the M3-P1 capacity
matrix. Its `slow-broadcast-recovery` scenario combines real TCP slow readers,
`TcpTransportEndpoint`, `BroadcastRouter`/`BroadcastDispatcher`, hierarchical
TCP output budgets, retained Buffer capacity, fixed network storage, and
process RSS in one run.

It is opt-in benchmark tooling, not CTest and not an installed target. The
versioned stdout document is `gamenet.capacity_profile.v1`.

## Contract

One run has three phases:

1. Connect the configured number of real TCP clients, set each receive buffer
   to 4 KiB, and do not read.
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
correctness or performance threshold.

`TcpConnection::memoryRetentionSnapshot()` is owner-loop-only. The profile
posts each low-frequency sample through the endpoint owner executor; it never
reads connection-owned Buffer state directly from its driver thread.

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

## 2026-07-30 local M3-P1 seed

The first Windows MSVC Release run used the command above. It is a local
capacity-matrix seed, not candidate/release evidence and not a regression
threshold.

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
