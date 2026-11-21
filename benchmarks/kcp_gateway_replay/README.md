# KCP game gateway replay

This standalone Release benchmark replays traffic derived from the installed-package
consumer `gamenet-game-gateway@4e2457e81f0ba2154b6aca2e6cd945daf793fbba`.
The workload profile pins the gateway source and the Q5
evidence commit. CMake embeds the profile hash in the executable, and the runner
archives the same profile for readback. [Workload profile](gateway-q3-q5.json)

## Workload

| Gateway input | KCP replay |
| --- | --- |
| Q3 broadcasts to two I/O owners with one slow reader | Two IPv4 loopback recipients on separate server loops, with one impaired peer |
| Q3 allows 256 slow replies with 3,900-byte bodies | 256 offers to the slow peer per epoch, yielding 3,910–3,913 byte messages in measure mode |
| Q3 uses a 128-byte oversized body and 64-byte dispatch task cap | One policy probe per epoch. The reader counts the two typed task-byte rejections separately |
| Q5 `scene:0` broadcast and 50 ms tick | A 37–40 byte sequenced broadcast every 50 ms in measure mode |
| Q5 completes 3,743 generation replacements in 3,600.015 seconds | One replacement per 961,799 µs epoch, about 1.040/s |

The relay drops server-to-client datagrams for the slow peer from +100 ms to
+600 ms per epoch. The pressure burst starts at +150 ms and the policy probe
at +75 ms. On recovery the coordinator closes the old KCP sessions, restores the
path, waits for both closes, opens a fresh conversation and replaces the player
binding. It sends a recovery probe through the new binding. The 500 ms fault
window and the combination of Q3 and Q5 are benchmark settings.

The gateway inputs are Q3 `tests/integration/test_queued_event_output_shutdown.cpp`
around lines 273–400 and Q5 `tests/integration/test_gateway_replay_fault_endurance.cpp`
around lines 384–441. `tests/fixtures/q5_gateway_replay_v1.trace` supplies the Q5
inputs and `src/queued_event_gateway.cpp` defines the body formatting. The source
files and hashes are recorded in the profile. The replacement count and elapsed
time come from `docs/evidence/m3-q5-1h-2026-08-23-4e2457e/result.json` at the
pinned evidence commit.

## Run and read back

Use Python 3.10 or newer and a C++23 toolchain on Windows or Linux. From the
repository root, build and install a Release GameNet package before running the
benchmark. Run the platforms sequentially on a shared host.

```sh
cmake -S . -B build/gateway-package -DCMAKE_BUILD_TYPE=Release -DGAMENET_ENABLE_UDP=ON -DGAMENET_ENABLE_KCP=ON -DGAMENET_BUILD_TESTING=OFF
cmake --build build/gateway-package --config Release --parallel 2
cmake --install build/gateway-package --config Release --prefix out/gateway-package
python tools/run_kcp_gateway_replay.py --package-prefix out/gateway-package --build-dir build/gateway-replay --output out/gateway-evidence
python tools/run_kcp_gateway_replay.py --verify-output out/gateway-evidence
```

Run these commands separately in the Windows and Linux environments using
distinct build, package and evidence directories when they share a checkout.
Use the same architecture and compatible toolchain for the package and
benchmark. Pass `--cmake` if the benchmark needs a specific CMake executable.
The default `measure` mode runs three 64-epoch samples with 20 warm-up broadcasts
each. Use `--mode smoke` for the single three-epoch sample used by both Release
CI lanes. The standalone build consumes the package's installed public targets.
All replay traffic uses local loopback.

Each evidence directory contains the profile, runtime and build provenance,
per-sample manifests and raw CSVs, and `summary.json`. The reader verifies hashes,
the scheduled offers and dispatch totals, a unique terminal per admitted send,
payload and generation alignment, actual fault drops, fresh probe delivery and
final zero settlement. It recomputes the recorded summary from the CSVs with
`--verify-output`. Only the evidence directory and the readback script are needed
for this step.

Healthy P99 uses nearest rank over actual application receives, timed from
publication. The summary also gives schedule-to-receive latency and the
publication latency of offers scheduled inside and outside the fault window.
Reject rates divide recipient offers without admission by all recipient offers,
with ordinary broadcast, pressure sends and the policy probe reported
separately. Recovery measures the actual relay release to the application receive
of the fresh-generation probe and includes close, open and rebind. The summary
also records latency from the fault start and admitted sends without a receive.
