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

The manual workflow additionally exposes `candidate-waiver` and
`release-waiver`. They are not duration modes: they start no endurance process
and make no Linux/epoll runtime claim. They exist for an owner who elects not
to run the long-duration evidence. A waiver requires a single-line 12-500
character reason, records the dispatching GitHub actor as approver, and applies
only to its named promotion stage.

Production modes reject duration overrides. `smoke` accepts 1–60 seconds for
orchestration tests, but its result cannot pass either production verifier.
The 72-hour verifier also requires retained successful 24-hour evidence from
the same candidate SHA, platform, and backend and emits
`gamenet.production_endurance_pair.v1`.

Production results also record their exact GitHub workflow run id and rerun
attempt. The workflow refuses a production mode without an exact paired
capacity source:

| Promotion stage | Capacity source | Endurance source |
| --- | --- | --- |
| candidate | paired `candidate-10k` raw artifacts | current `candidate-24h` |
| candidate waiver | paired `candidate-10k` raw artifacts | none; explicit owner/reason metadata |
| release | paired `dedicated-100k` raw artifacts | exact prior `candidate-24h` plus current `release-72h` |
| release waiver | paired `dedicated-100k` raw artifacts | none; explicit owner/reason metadata covering 24h/72h |

`tools/verify_production_promotion_evidence.py` does not trust copied summary
fields. It revalidates both platform capacity manifests and raw v3 samples,
requires the checked-in `pair-manifest.json` to equal the recomputed pair,
revalidates every endurance result and hashed log, and binds all inputs to the
same candidate SHA and declared workflow attempts. The resulting
`gamenet.production_promotion_evidence.v1` is included in the long-soak
artifact and then hashed by its outer CI evidence manifest. Either waiver uses
the distinct `gamenet.production_promotion_waiver.v1` schema with
`status: waived`, `endurance_policy: owner-waived`, an empty endurance list,
`duration_evidence_complete: false`, and `owner_authorized_promotion: true`.

## Remote Runner Boundary

The production mode in `long-soak.yml` targets
`[self-hosted, linux, x64, gamenet-endurance]`. GitHub-hosted jobs are limited
to six hours, while self-hosted jobs may run for up to five days, so a genuine
72-hour single-process claim requires dedicated self-hosted infrastructure.
The workflow gives the job a 4,620-minute bound and avoids step-level timeouts.
It uses the GitHub token only before the long child run to download the exact
capacity run/attempt and, for release mode, the exact prior 24-hour
run/attempt. Capacity evidence is revalidated before the expensive
uninterrupted process starts.

The shared production-endurance waiver job is deliberately separate and runs
on `ubuntu-24.04`, because it only revalidates retained capacity artifacts and
writes waiver metadata. GitHub-hosted execution does not weaken a duration
claim here because the artifact explicitly makes no such claim.

Official limits:

- https://docs.github.com/en/actions/reference/limits
- https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax

Linux/epoll is the production endurance environment. Linux and Windows still
run the fault-injection CTest in ordinary CI, sanitizer, Release, and package
gates; Windows/IOCP is therefore a supported functional backend, but the 24/72
hour release-duration claim is explicitly Linux/epoll.

## Completed Infrastructure Evidence

The Phase 6 infrastructure snapshot
`b3443182d0606792df44a12bcb08927e767bc060` completed both unscaled modes on
the dedicated Linux/epoll runner:

- 24-hour candidate run `29895457789`: success, 73,617 uninterrupted cycles;
- 72-hour release run `29984629032`: success, 220,851 uninterrupted cycles,
  including verification of the retained same-SHA 24-hour artifact.

These records prove the supervisor, process-duration, RSS handshake, checkpoint,
hashing, and same-SHA pair machinery. They do not automatically qualify later
runtime commits. Any EventLoop, TCP, platform data-path, Metrics hot-path, or
other runtime change requires new 24/72-hour evidence on the final frozen SHA.

## Local Smoke

After a Debug or Release test build:

```text
python tools/run_endurance_gate.py --test-dir build --configuration Debug --mode smoke --duration-seconds 5 --candidate-sha <40-hex-sha> --platform windows --backend iocp --output-root build/endurance-smoke
```

The output directory must not already exist. A passing smoke validates the
driver, heartbeat parser, checkpoint writer, and evidence hashing only.
