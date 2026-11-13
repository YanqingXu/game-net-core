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
KCP, PMTU policy and raw ICMP stay deferred. Separate 2026-11-13 UDP-C2
authorization adds the bounded explicit peer layer below; the C1 socket API
and native cancellation contract remain unchanged.

## UDP-C2 explicit peer sessions and endpoint adaptation

UdpPeerServer composes UdpSocket on one outliving EventLoop. Factory, start,
openPeer, localAddress and snapshot are owner-only. No incoming packet creates
a peer. A bounded address index selects an explicitly opened live peer; unknown
sources are counted and dropped. IPv6 scope, address, family and port participate
in address identity. Each fixed-capacity slot has a monotonically advanced
generation; each opening also receives a nonzero TransportSessionId unique across
UDP-C2 server instances in this process. Application-assigned TCP identities are
not part of that allocator; C2 does not create a shared player/dual-channel map.
Exhausted identity counters refuse rather than wrap. Duplicate live address opens
reject. Closing peers occupy capacity until terminal settlement but no longer
receive; a free slot may open the same address before an old completion arrives.
Old handles/tasks/receipts retain their original identity and cannot mutate a
replacement. Raw UDP has no authenticated identity or wire generation: delayed
packets from the same source address cannot be classified as old/new sessions.

UdpPeer implements existing TransportEndpoint without modifying its interface.
send/sendDatagram/close run on owner; requestClose and isOpen are thread-safe.
Owner executor remains safely observable after loop teardown. Handles weakly
observe server state and do not extend loop lifetime. Closed handles report closed;
live foreign sends reject. Endpoint send maps packet-size/resource refusal to
Overloaded; direct sendDatagram preserves InvalidArgument/NoMemory results.
Accepted means copied local admission; Sent means OS completion, never delivery.
close has local UDP semantics, not a wire close handshake or remote EOF.

Count AND byte limits apply per peer and globally, including the one shared native
in-flight packet; empty packets consume a count slot. A ready-peer FIFO chooses one
packet per peer per round. A single coalesced bounded normal owner post drives
data; initial scheduling refusal rolls back admission, later scheduling failure
seals service and cancels pending output. No data uses lifecycle priority to evade
normal admission. Receive work is already bounded by C1. Idle checks and terminal
cancellations have finite work-per-turn limits. Idle time measures admitted send
or delivered receive, uses steady time, and does not refresh on rejected work.

Peer requestClose seals admission under the same synchronization as send and
datagram callback commitment, records first reason and signals the pre-attached
lifecycle node without normal-queue allocation. Already committed callback frames
may finish. Stop/quit seal all peers and native socket. Queued output cancels;
an already submitted packet waits for its actual C1 terminal. Per-send callbacks
release count/byte reservations before invocation and occur exactly once for
accepted ids, zero for rejected work. Per-peer closed occurs once after zero
pending; server stopped follows all peers, timer/lifecycle retirement and native
socket Stopped with zero operations/storage. Created -> Running -> Stopping ->
Stopped is one-shot; stop before start has no user callbacks.

Caller owns wrapper; active state retains a settlement lease. Slots own peer
records, public handles weakly observe state. Timers/normal posts/native bridges
observe weakly; dispatch takes a state lease across callbacks. Callbacks may
send/close/open/stop/throw/release wrapper or handles. Exceptions seal service;
remaining obligations still settle. One callback cannot recursively dispatch the
same receipt; old completion identity is ignored after its terminal removal.

Direct contract: `tests/contract/udp/test_udp_peer.cpp` (identity, generations,
caps, idle, bounded scheduling refusal, cross-thread close under saturation,
reentry/exception/release, callback and packet/byte ledgers, weak expiry).
Real fairness/reconnect: `tests/integration/udp/test_udp_peer_stress.cpp`.
Bounded sequences share tests/support/UdpPeerSequence.h and
`tests/fuzz/udp/fuzz_udp_peer.cpp`: independent per-id terminal ledger, stepwise
count/byte ceilings, old receipts/handles, interleavings and final zero residue.
Test-only receive/idle/late-completion controls are not installed, never replace
real Windows/Linux native completion or independent dual-family peer evidence.
All profiles retain exact inventory, hashes, raw results and 20,000 fuzz units.
`tests/ci/run_udp_peer_fuzz.py` retains and independently rechecks raw bounded
fuzz evidence; `tests/ci/test_udp_peer_fuzz.py` rejects count/seed/diagnostic drift.

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
