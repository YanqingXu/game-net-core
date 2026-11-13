---
status: active
target: GameNet::udp
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# UDP-C1: bounded datagram socket and minimal server

The 2026-11-12 user authorization enables provisional GameNet::udp, default OFF,
linking only Core. Existing Core API, TCP/TLS/WS behavior, releases and HP0 gates
stay unchanged. This native contract supersedes the historical UDP design;
KCP, peer/session maps, PMTU policy and raw ICMP stay deferred.

## Owner and lifetime

One EventLoop owns the socket, Channel, operations, callbacks and snapshots.
Factory/start/snapshot are owner-only. Wrapper destruction requests stop safely. sendTo and stop
provide thread-safe typed admission; foreign callers never invoke user code.
The loop outlives the socket and drains stop. Each generation starts once;
repeat service runs use fresh objects and independent immutable ledgers.
The wrapper owns state; active state has an explicit lease until settlement.
Lifecycle/quit callbacks observe state weakly. Datagram/send/stopped callbacks
may send, stop, throw or release caller references. Dispatch leases state.
Exceptions stop explicitly without skipping accepted-send settlement. No new datagram callback is committed after the stop seal; a callback frame
committed under the admission lock before stop may finish. Removed current Channels retire after dispatch.
Stopped occurs once after zero reservations, queue and native operations.

## Admission and budgets

Every admitted send owns one copied datagram and nonzero id. A mutex protects
packet AND byte reservations, including in-flight output; empty datagrams still
consume packet capacity. Rejected sends retain nothing and have no completion.
Accepted output terminates once as Sent, Cancelled or Error, after reservation
release and before stopped. Sent means local OS completion, not peer delivery.
For packets and bytes: accepted = sent + cancelled + failed + pending.
No fallback queue/task follows capacity or lifecycle-admission refusal.
Read and send turns have packet/byte budgets; byte budgets fit one maximum
configured datagram. Linux recvmsg/sendto returns at would-block or budget.
Windows uses one WSARecvFrom and one WSASendTo at a time, real IOCP identity,
storage leases and bounded owner completion/lifecycle phases. Operation reuse
waits for terminal consumption. Stop/quit cancellation retains OVERLAPPED,
address, buffer and payload storage through late completion and revoked
observers. A source-private access seam adds no public Core operation/API.

## Datagram semantics

IPv4/IPv6 preserve family and datagram boundaries; IPv6 sets IPV6_V6ONLY.
Empty receive is data, never EOF. Finite receive storage yields a bounded
prefix plus explicit truncated flag; original length is not invented. Linux
uses recvmsg MSG_TRUNC, Windows recognizes WSAEMSGSIZE completion. Subsequent
packets remain usable. Oversize/family mismatch output rejects atomically.
Permanent receive/submission failure stops explicitly. ICMP errors do not
acquire TCP connection semantics. Callback exceptions cannot strand cleanup.

## Verification

- tests/unit/udp/test_udp_options.cpp: option bounds and invalid setup.
- tests/contract/udp/test_udp_socket.cpp: IPv4/IPv6, empty/exact/oversize packets,
  truncation/recovery, packet/byte admission, foreign send/stop, executor
  saturation, reentry/exception/release, quit, IOCP cancellation, zero residue.
- tests/integration/udp/test_udp_stress.cpp: real bursts must refuse then
  recover, timer/second socket fairness, repeated fresh starts/stops and weak
  generation expiry without stale callbacks.
- Independent Python UDP peers use both families, binary/empty payloads and
  the minimal echo server. Execute Windows/Linux Release, ASan/UBSan/TSan,
  exact inventories, ON/OFF installed isolation and all existing regressions.
Short stress provides correctness evidence, not capacity/endurance promotion.

The exact evidence driver tests/ci/run_udp_contracts.py and negative-evidence
guard tests/ci/test_udp_workflow.py run in the dedicated optional UDP workflow.

Stop before start releases the unregistered socket/storage and returns Stopped
without callbacks. A stop racing start seals admission; start commits a final
lifecycle signal so no request is lost. After registration removal, socket close
ensures native cancellation progresses while operation/payload leases remain.

The socket state machine is Created -> Starting -> Running -> Stopping -> Stopped,
with direct Created -> Stopped on an unstarted stop. Starting seals concurrent
stop without allowing callbacks or native storage to escape registration.

Connection-refused/reset receive indications are per-datagram ICMP diagnostics,
counted without inventing a peer or closing the shared socket; later valid
datagrams must recover. Other permanent receive failures stop explicitly.

A deterministic pre-callback hook in the datagram contract seals from a foreign
thread between native receive and callback commitment and verifies no callback
is committed, while native storage still retires to zero.
