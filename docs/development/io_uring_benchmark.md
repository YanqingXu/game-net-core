# IOE-X1 io_uring Directional Benchmark

The IOE-X1 benchmark drives the Linux-only, default-off raw io_uring Engine
through a finite pipeline of one-shot Send and Recv operations. It is
directional development evidence. It does not replace the production Core
epoll benchmark and is not release or promotion evidence.

Build it explicitly on Linux:

```bash
cmake -S . -B build-io-uring-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAMENET_BUILD_TESTING=OFF \
  -DGAMENET_BUILD_BENCHMARKS=ON \
  -DGAMENET_ENABLE_EXPERIMENTAL=ON
cmake --build build-io-uring-release --parallel \
  --target gamenet_io_uring_one_shot_benchmark
```

Run one sample with 100,000 round trips, a 256-byte payload, and 64 operations
in flight:

```bash
./build-io-uring-release/benchmarks/gamenet_io_uring_one_shot_benchmark \
  100000 256 64 > build-io-uring-release/io-uring-sample.json
python3 tools/validate_io_uring_benchmark.py --require-release \
  build-io-uring-release/io-uring-sample.json
```

The `gamenet.io_uring_one_shot_benchmark.v1` document reports message and
operation rates, bidirectional MiB/s, P50/P99/P999 round-trip latency,
working-set delta, shutdown latency, accepted/terminal operation counts,
SQ-full rejection, fallback, and residual Engine state. A valid successful
sample has exactly two accepted and terminal operations per round trip, no
fallback or SQ rejection, and no active operation, ready notice, or owned byte
after the drained shutdown.

The peer uses a real finite `SOCK_SEQPACKET` socket. The owner thread keeps at
most the configured depth in flight and consumes every typed terminal notice;
there is no callback, readiness, inline, or unbounded-overflow fallback.

## IOE-X5 shared-Pump TCP Hub benchmark

IOE-X5 adds a separate opt-in benchmark for the fixed-capacity shared Hub. It
uses 256 real loopback TCP connections by default, keeps one 64-byte echo frame
in flight per route for 100 round trips, and validates exact connection,
operation, byte, rejection, and residual-state accounting.

```bash
cmake -S . -B build-io-uring-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAMENET_BUILD_TESTING=OFF \
  -DGAMENET_BUILD_BENCHMARKS=ON \
  -DGAMENET_ENABLE_EXPERIMENTAL=ON
cmake --build build-io-uring-release --parallel \
  --target gamenet_io_uring_shared_tcp_hub_benchmark
./build-io-uring-release/benchmarks/gamenet_io_uring_shared_tcp_hub_benchmark \
  256 100 64 > build-io-uring-release/shared-hub-sample.json
python3 tools/validate_io_uring_shared_hub_benchmark.py --require-release \
  build-io-uring-release/shared-hub-sample.json
```

The `gamenet.io_uring_shared_tcp_hub_benchmark.v1` document reports round-trip
rate, bidirectional throughput, P50/P99/P999 latency, active working-set delta
and bytes per connection, shutdown latency, high-water marks, rejection
counts, and zero-residue metrics. It is deliberately outside CTest.

Five local WSL Ubuntu 24.04/GCC 13.3 Release samples at 256/100/64 produced
medians of 218,581 round trips/s, 26.682 MiB/s, 1,163/1,270/1,354 us
P50/P99/P999, 3,552 active bytes per connection, and 0.866 ms Hub shutdown.
Five production epoll Core samples at the same connection/message/payload
counts produced 99,317 round trips/s, 12.124 MiB/s, 2,467/4,447/5,682 us
P50/P99/P999, 5,600 bytes per connection, 1.652 ms connection close, and
0.102 ms server shutdown.

These numbers are directional only: the shared-Hub probe uses one poll-based
echo peer, while the Core probe uses the production server/client-worker
topology. They do not establish backend superiority or a regression threshold.

**IOE-X5 decision: `PROMOTE`.** Promotion is narrowly scoped to the next
source-private production-adapter contract-shaping slice. The public backend
selector and production `TcpConnection` remain unchanged until equivalent
cross-backend semantics, failure behavior, and repeatable gates are proven.
