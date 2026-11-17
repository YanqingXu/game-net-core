---
status: active
target: GameNet::kcp_transport
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# KCP-C2: endpoint composition and two-process echo examples

## Authorization and invariants

The user authorized KCP-C2 on 2026-11-16. Add the separate optional
GameNet::kcp_transport composition of GameNet::kcp and GameNet::transport when
KCP/UDP are enabled. KCP remains Core/UDP-only; neither Core, UDP nor transport
depends on KCP. No OpenSSL, new wire envelope, discovery, handshake, PMTU,
authentication, performance promotion or release is authorized by this slice.
Existing public Core, UDP, KCP and TransportEndpoint contracts stay unchanged.

KcpTransportEndpoint owns only a KcpSession handle, which weakly observes its
transport. It extends neither transport nor EventLoop lifetime and registers no
callbacks, timer or queue. Null construction throws invalid_argument. Endpoint
objects are shared through pointers, not copied or moved; no moved-from endpoint
can lose its required session handle. Identity
uses the immutable globally unique UDP peer id, never conv alone. Identity,
ownerExecutor and isOpen remain safe across threads and after owner teardown.
The application owns/releases adapters; KCP retains responsibility for pending
settlement and native retirement, including callback reentry and wrapper release.

## Thread and result contract

send/close execute only on the session owner; applications marshal owned bytes
through ownerExecutor and handle rejected posts. The adapter delegates without
adding scheduling or changing result precedence: a sealed/unavailable session
returns Closed; an open session called from a foreign thread returns WrongThread.
Accepted maps to Accepted, Stopped to Closed, WrongThread to WrongThread,
OwnerUnavailable to OwnerUnavailable, and InvalidArgument/NoMemory/Overloaded
to Overloaded. send copies into existing bounded KCP storage; Accepted is local
admission, not an ACK or remote application delivery promise.

CloseReason Normal/GoingAway/ProtocolError/IdleTimeout map to the corresponding
KcpCloseReason. Replaced maps to GoingAway (local cancellation), and Overloaded
maps to ResourceExhausted. requestClose delegates the generation-bound lifecycle
capability from any thread, including ordinary queue saturation. First close
reason wins; stale adapters cannot send to or close replacements. Close is local,
without a remote close handshake. Existing KCP callbacks may reenter and release
adapters; adapter operations introduce no new callback path.

## Examples and package contract

Two separate bounded processes, gamenet_kcp_echo_server and
gamenet_kcp_echo_client, use the endpoint interface for sends and explicit local
and remote numeric addresses/ports plus a fresh nonzero conv. Both families,
empty messages and fragmented payloads are supported. The client keeps serving
protocol ACKs until its configured finite deadline; the server succeeds only
after its echo is acknowledged. Both stop and drain through stopped, report
actual accepted/acknowledged/cancelled/failed counts, and require zero pending,
protocol, post and native storage. Invalid options and no-response deadlines
produce nonzero exits, not an unbounded wait or synthetic success.

Installed kcp_transport discovery imports its KCP/UDP dependencies only when
requested. Core-, UDP-, and KCP-only consumers never load the adapter or OpenSSL.
Physical OFF packages expose neither its headers, library nor export and reject
required discovery with the correct component diagnostic.

## Verification

- tests/contract/kcp/test_kcp_transport_endpoint.cpp: constructor, identity,
  real payload exchange, bounded refusal, foreign send/close, saturated lifecycle
  close, every close-reason mapping, first-reason preservation, callback release,
  replacement isolation, teardown and exact zero-residue settlement.
- tests/ci/kcp_examples.py: run both actual example binaries over IPv4/IPv6,
  empty/fragmented messages, invalid options and deadline failure; retain commands,
  exits and raw summaries. No skipped/fallback case counts as passed.
- tests/ci/run_kcp_contracts.py: all five existing Windows/Linux sanitizer lanes,
  unchanged independent upstream interoperability, installed ON/OFF consumer
  isolation and the new dual-process examples.
- tests/ci/run_clang_tsan_gate.py: complete 142-test inventory, existing custom-new
  semantics, atomic exit 0 and expected heap-race exit 66, with independent readback.
- tests/fuzz/kcp/fuzz_kcp_protocol.cpp and tests/fuzz/kcp/fuzz_kcp_session.cpp:
  preserve bounded 20k-per-target protocol/state regression and original oracles.

Repository scope/API/intent/provenance guards enforce the composition boundary.
Existing Core/UDP/TLS/WS/WSS/RPC/HP1 regression gates remain required. Evidence
records the actual source/worktree and toolchain; no claim of clean-commit or
hosted CI validation may be made from a dirty local run.
