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
