# Production Fault Injection and Endurance

Phase 6 adds one fault-injection integration executable that can run once as a
normal CTest contract or remain alive for a fixed production endurance
duration. Every cycle injects the same five profiles: abrupt peer reset,
message-callback exception, output hard-limit rejection, a healthy recovery
request, and slow-reader graceful-stop timeout.

## Runtime Contract

`tests/integration/resilience/test_fault_injection.cpp` owns its raw client
sockets. TcpServer and TcpConnection retain their existing base-loop and
connection-loop ownership. The driver uses only public cross-thread-safe
requests, and every injected failure converges through the normal
close/remove-before-destroy paths.

The CTest default performs one cycle. `tools/run_endurance_gate.py` resolves the
registered executable from CTest, hashes it, then launches one uninterrupted
child process with a fixed duration. It validates monotonic JSON heartbeats,
the exact fault-profile inventory and counters, bounded heartbeat silence,
process exit, child-reported duration, and independently observed wall time.
It atomically rewrites `checkpoint.json` after each cycle and produces
`gamenet.production_endurance.v1` with a hashed raw log.
On Linux it also samples the same child process RSS after every heartbeat and
acknowledges the observation before the child may continue. This handshake
keeps even the final heartbeat's process alive while `/proc` is sampled and
fails closed if either side disappears. The gate fails if maximum RSS exceeds
512 MiB or end-to-first growth exceeds 64 MiB.

## Fixed Modes

- `candidate-24h`: exactly 86,400 seconds;
- `release-72h`: exactly 259,200 seconds.

Production modes reject duration overrides. `smoke` accepts 1–60 seconds for
orchestration tests, but its result cannot pass either production verifier.
The 72-hour verifier also requires retained successful 24-hour evidence from
the same candidate SHA, platform, and backend and emits
`gamenet.production_endurance_pair.v1`.

## Remote Runner Boundary

The production mode in `long-soak.yml` targets
`[self-hosted, linux, x64, gamenet-endurance]`. GitHub-hosted jobs are limited
to six hours, while self-hosted jobs may run for up to five days, so a genuine
72-hour single-process claim requires dedicated self-hosted infrastructure.
The workflow gives the job a 4,620-minute bound and avoids step-level timeouts.
It uses the GitHub token only before the long child run when a 72-hour run must
download its prior 24-hour artifact.

Official limits:

- https://docs.github.com/en/actions/reference/limits
- https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax

Linux/epoll is the production endurance environment. Linux and Windows still
run the fault-injection CTest in ordinary CI, sanitizer, Release, and package
gates; Windows/IOCP is therefore a supported functional backend, but the 24/72
hour release-duration claim is explicitly Linux/epoll.

## Local Smoke

After a Debug or Release test build:

```text
python tools/run_endurance_gate.py --test-dir build --configuration Debug --mode smoke --duration-seconds 5 --candidate-sha <40-hex-sha> --platform windows --backend iocp --output-root build/endurance-smoke
```

The output directory must not already exist. A passing smoke validates the
driver, heartbeat parser, checkpoint writer, and evidence hashing only.
