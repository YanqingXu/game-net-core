# Optional bounded standard KCP (KCP-C1/C2/C3/C4)

## KCP-C4 development validation

The C4 intent is `intents/usecases/kcp_capacity_endurance.intent.md`. The tool
defaults OFF and is neither installed nor exported. Its owner loop creates real
KCP sessions and bounded native UDP forwarding sockets; the Python supervisor
only owns the child, logs, deadlines and external process-memory observations.
There is no second proxy queue or automatic reconnect policy.

```sh
python tools/run_kcp_capacity_endurance.py --profile release --build-dir build-kcp-c4 --output out/kcp-c4-smoke
python tools/run_kcp_capacity_endurance.py --profile release --mode soak --build-dir build-kcp-c4 --output out/kcp-c4-development-30m
python tools/run_kcp_capacity_endurance.py --verify-output out/kcp-c4-development-30m
```

Use a fresh output directory. The runner explicitly enables KCP, UDP and
`GAMENET_BUILD_KCP_VALIDATION=ON`; `--profile asan` and `--profile tsan` select
instrumented smoke (TSan is Linux-only; `--compiler clang++-18` selects the full
gate's toolchain). Windows uses the MSVC ASan runtime next to its compiler.
Smoke runs all six 1/8/32-session IPv4/IPv6 profiles. Soak uses one uninterrupted
N=32 process for at least 1800 seconds, alternating complete IPv4/IPv6 cycles;
the current cycle finishes before final settlement. Run soak on both Windows
and Linux. Sanitizer smoke does not count as a 30-minute development run.

To run both native Release samples on GitHub Actions, select **Optional bounded
standard KCP** under Actions, choose the branch, and enable `c4_soak`. With
GitHub CLI, use `gh workflow run kcp.yml --ref <branch> -f c4_soak=true`.
Both jobs check out the dispatch commit and run for at least 1800 seconds.
Each uploads its raw evidence after a separate reader invocation. Download an
artifact and run `python tools/run_kcp_capacity_endurance.py --verify-output <extracted-directory>`
to repeat the check.

Every cycle observes full pending pressure, refuses excess sessions/messages,
drops actual data and ACK datagrams, requires healthy-peer progress during a
blackhole, restores ordered empty/fragmented messages, times out an unreachable
send, rejects stale/reused identities, exchanges a fresh generation, cancels a
local pending send, exhausts conv history and refuses a second protocol arena.
Each accepted id has exactly one local terminal; remote receives are counted
separately. A complete cycle releases all protocol/pending/native UDP storage
and lifecycle nodes before the next RSS sample. Only current-cycle ledgers live
in the child. Conv values are explicit test configuration, without authentication.

`manifest.json` uses `gamenet.kcp_capacity_endurance.v1`. Raw child JSONL includes
its real PID, backend, configured bounds, monotonic cycle/time, exact ledgers,
arena/pending/proxy peaks, RSS/OS high water and zero-residue result. External
PID RSS samples, UTC start/end, commands, compiler configuration, source-input
copies, binary and SHA-256 file inventory accompany it. Release ceilings are
512 MiB peak and 64 MiB post-warmup retained RSS growth; sanitizer RSS remains
diagnostic. Missing records, restarts, shortened duration, altered limits,
duplicates, nonzero exit/residue and tampering fail independent readback.
Failed/interrupted runs retain checkpoints and are never promoted to success.

The existing five KCP lanes, full 143-test Clang 18 TSan gate and allocator
controls remain intact; C4 smoke is an additional step. Existing upstream
interoperability, 20k protocol/session fuzz and installed consumers remain
required. C4 evidence is developmental: it does not establish production
capacity, complete TCP candidate-1h/release-3h gates, promote HP0, or issue a release.

KCP-C3 composes actual endpoints with SessionManager, LogicLoop and broadcast
across distinct network/management/logic loops. Bindings and owned payloads cross
loops; mutable PlayerSession views do not. Broadcast plans/tasks keep revocable
bindings and check them at dispatch and immediately before send. A refreshed or
rebound generation produces OfflineSession rather than a stale send, including
when the same endpoint remains open. Admission is distinct from protocol ACK and
remote receive. The dual-family test forces pending/task/byte/queue refusal,
healthy-owner progress, stale logic, delayed offline and saturated shutdown with
an unacknowledged send, then verifies exact terminal accounting and zero residue.
See tests/integration/kcp/test_kcp_game_layers.cpp and the C3 intent/rules.
The same CTest entry runs the churn case once per family. It drops the
impaired peer's return ACKs while data continues to arrive and repeats
same-endpoint refresh before replacing the session on another network loop.
Each refresh, replacement and explicit close has queued logic and broadcast
work, with a healthy recipient on a separate owner. The end of each round
checks local terminals, remote messages, and zero retained work, timers and
native storage before reusing the same owner loops.

For a 30-minute Release run, build with UDP and KCP enabled. Use
`ctest --test-dir <build> -C Release -N -V -R "^integration.kcp.test_kcp_game_layers$"`
to locate the test executable, then run it with `--churn-soak`.
Each `KCP_C3_CHURN` record reports the completed round and address family.
The final `KCP_C3_SOAK` line records the actual monotonic duration and round
count. Use the default CTest command for the short sanitizer run.

Evidence schemas gamenet.kcp_contracts.v3 and gamenet.clang_tsan_gate.v3 require
the actual two-family C3 summaries and reject absent/partial records. Current
enabled inventory is 143; focused UDP/KCP sanitizer inventory is ten; default is
133. The gate verifies C3 is TSan-instrumented as well as the allocation-fault
contract. No new installed signature or production/performance claim is added.

The game gateway replay runs the pinned Q3/Q5 workload against a Release install
on Windows or Linux. Its
archived profile and raw ledgers support independent readback of healthy P99,
per-class refusal and recovery to the first new-generation application receive.
Both KCP Release lanes verify the installed-package smoke sample.
[Game gateway replay instructions](../../benchmarks/kcp_gateway_replay/README.md)

`GameNet::kcp` is a provisional message transport over explicit UDP peers. Build
with both `-DGAMENET_ENABLE_KCP=ON -DGAMENET_ENABLE_UDP=ON`; both default OFF.
There is no OpenSSL, custom wire header or TransportEndpoint dependency.
The separately requested `GameNet::kcp_transport` component adapts the existing
TransportEndpoint interface; KCP itself retains its Core/UDP-only dependency.

```cmake
find_package(GameNetCore CONFIG REQUIRED COMPONENTS kcp_transport)
target_link_libraries(your_server PRIVATE GameNet::kcp_transport)
```

`gamenet::kcp_transport::KcpTransportEndpoint(session)` retains a session handle,
not its transport or loop. Its id is the globally unique peer id; equal convs
in different transports do not collide. Null construction throws. `send` and
`close` remain owner-only; foreign callers post owned data through ownerExecutor.
`requestClose` uses KCP's lifecycle lane even when the normal queue is full.
The adapter changes no callbacks. Accepted is bounded copied-message admission;
ACK and close retain the C1 semantics described below. Closed/unavailable
sessions report Closed before a thread check, while an open foreign call reports
WrongThread. Invalid/oversize, allocation or capacity refusal reports Overloaded.
Normal, GoingAway, ProtocolError and IdleTimeout map directly; Replaced maps to
GoingAway/local cancellation, Overloaded to ResourceExhausted. First close wins.

The build also provides two finite examples (run each command in its own terminal):

```sh
build/examples/gamenet_kcp_echo_server 127.0.0.1 20000 127.0.0.1 20001 1001 10000
build/examples/gamenet_kcp_echo_client 127.0.0.1 20001 127.0.0.1 20000 1001 "hello KCP" 3000
```

MSVC binaries are under `build/examples/Release/` with `.exe`. Both sides must
agree on explicit addresses/ports and a fresh nonzero conv; use `::1` at both
ends for IPv6. An empty quoted message is valid; messages up to the configured
16 KiB bound are fragmented as needed. The optional final argument is a finite
deadline in milliseconds (100..60000). Client defaults to 3000, server to 10000;
client waits through its window to service ACKs for the echo, since KCP has no
remote close handshake. Server succeeds after its echo is acknowledged. Set the
server deadline longer than the client's window. Missing responses, incorrect
payloads, refusals or incomplete settlement produce nonzero exits. Both sides
stop/drain to `stopped` and print a `KCP_ECHO_SUMMARY` with actual terminal counts
and a zero-residue flag. These examples are explicit-address diagnostics, with
no authentication, automatic peers or production/performance claim.

```cmake
find_package(GameNetCore CONFIG REQUIRED COMPONENTS kcp)
target_link_libraries(your_server PRIVATE GameNet::kcp)
```

Create/start on an owner EventLoop, exchange the known remote UDP address and
nonzero conv out of band, then call `openSession(address, conv)` at both ends.
The address/conv is not authenticated. Select a fresh conv for each replacement,
including across process restarts. Each transport refuses conv reuse and caps
its history; recreate it with a fresh application-managed conv namespace after
history exhaustion. There is no peer discovery or KCP close handshake.

`session->send(payload)` accepts complete copied messages, including empty ones,
and returns an id. Rejection returns id zero with no reservation or callback.
Accepted messages terminate exactly once locally: Acknowledged means cumulative
KCP ACK passed all fragments, Cancelled means local close/stop, Failed means a
deadline, resource, scheduling or transport failure. ACK does not promise remote
application processing. Close preserves already committed ACK results.

Creation/start/open/send/close and snapshots are owner-only. `requestClose`,
transport stop and wrapper destruction may run cross-thread and seal admission
before returning. Callbacks run on owner outside locks and native KCP; they may
reenter/throw/release. Destroying a running wrapper requests stop. Keep the owner
alive and draining until stopped, which includes UDP/IOCP terminal retirement.
Old handles never acquire a replacement generation.
Owner cleanup retires both KCP and UDP idle timers even after quit seals public
timer admission. If UDP expires first, its IdleTimeout is preserved in KCP;
local quit remains GoingAway with Cancelled sends. An earlier KCP seal wins.
The same cancellation rule applies when a message or ACK completion callback
stops/quits, throws or releases the last wrapper while another send deadline is
already due; already committed ACKs remain Acknowledged.

Default MTU 1200, send window 32, receive window 128, send-segment cap 64 and ACK
cap 256 are validated. Message size defaults 16 KiB, per-session pending count
32/bytes 64 KiB, aggregate count 256/bytes 1 MiB, sessions 32, conv history 4096,
protocol arena aggregate 16 MiB. Each engine reserves its entire fixed arena and
scratch up front (about 440 KiB at defaults; exact size depends on ABI). Pending
metadata and UDP copied/in-flight output have independent count/byte limits.
Empty messages consume count and segment capacity. Configured bounds include
closing sessions until their original native obligations retire.

Input validates the whole standard datagram before mutation. ACK pressure drops
it for retransmission. ACK timestamps older than the maximum upstream RTO of
60 seconds are rejected before the signed RTT filter can overflow. UDP output
refusal is wire loss with no secondary queue.
The standard congestion controller remains enabled. Default update interval is
20 ms, send lifetime 10 seconds, idle timeout 30 seconds, and retransmission limit
10. An unreachable peer eventually fails; the result cannot disambiguate remote
delivery followed by lost ACK from no delivery. Reconnect/retry policy is the
application's responsibility.

The unchanged MIT upstream is fixed at
[`b1a7a2101dcbb96017681a500d6b82bbe5a88766`](https://github.com/skywind3000/kcp/commit/b1a7a2101dcbb96017681a500d6b82bbe5a88766).
`third_party/kcp/upstream.json` records exact raw-byte hashes, including original
line endings. Private symbol prefixes isolate allocator hooks from another
upstream copy. The unchanged source compiles as C++ so fixed-arena exhaustion
unwinds to the adapter. Its C null-base container-offset macro is overridden
with standard `offsetof` in compilation wrappers (including the independent
reference); no algorithm or wire format changes and no sanitizer suppression.
Interrupted native mutation
discards the entire arena without following partial lists. MSVC disables `/EHc`
for this target so allocator exceptions cross the private C-linkage boundary.

Verification uses five CTest tests (143 with UDP, versus default 133), real
IPv4/IPv6 pressure/reconnect, a separate unchanged-upstream/native-socket peer
through bounded bidirectional data/ACK loss, duplication, reordering and delay,
and protocol/session fuzz at 20k each. The dedicated workflow records Windows/
Linux Release, Windows ASan, Linux ASan/UBSan and TSan evidence, package consumers,
clean source identities and all raw hashes. Existing optional protocol checks
remain separate. These contracts are not capacity or production validation.

The independent `clang-tsan-full` job additionally runs the complete 143-test
UDP/KCP-enabled inventory with Clang 18 on pull requests and main pushes. It
keeps the existing GCC TSan lane. An external allocation control uses the same
complete shared TSan runtime as the custom-new TCP contract: atomic accesses
must exit 0, and a deliberate heap race must report the custom-new/malloc stack
and exit 66. The probe is outside CTest and does not increase its inventory.
No test filtering, skipping or sanitizer suppression satisfies this gate.

Run `python3 tests/ci/run_clang_tsan_gate.py --compiler clang++-18 --build-dir
build-clang-tsan --output out/clang-tsan-evidence`, then independently read it
back with `python3 tests/ci/run_clang_tsan_gate.py --verify-output
out/clang-tsan-evidence`. Linux needs Clang/LLVM 18 and its compiler-rt package;
the workflow installs these explicitly. Evidence retains source/run identity,
toolchain/cache/compile commands, all 143 JUnit records, raw commands/logs,
runtime linkage, probe binary/diagnostics and SHA-256 file identities. Empty,
wrong-compiler, incorrectly successful race, suppressed or tampered evidence
fails the readback. `DEBUGINFOD_URLS` is empty during the run so symbolization
uses the installed LLVM toolchain without waiting for remote debug downloads.
