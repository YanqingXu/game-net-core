---
status: active
target: GameNet::udp_transport
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# UDP-C2: optional endpoint composition

The 2026-11-13 UDP-C2 authorization includes this narrow adapter. The UDP peer
library remains Core-only at both include and link levels. GameNet::udp_transport
depends on GameNet::udp and GameNet::transport and is built only when UDP is ON.
Its separate export is discovered only when explicitly requested; UDP-only and
Core-only discovery never load it or OpenSSL. OFF rejects the component explicitly.

UdpTransportEndpoint owns one UdpPeer handle which weakly observes server state;
it extends neither server nor EventLoop lifetime. Identity wraps the immutable
UDP-local id in TransportSessionId. ownerExecutor/isOpen remain safe after owner
teardown. send/close are owner-only and map exact UDP results to existing endpoint
results; size/NoMemory refusal maps to Overloaded. requestClose forwards the
generation-bound lifecycle capability and first reason, including saturation.
Close reasons map by explicit enum cases, without remote-delivery/close promises.
The adapter installs no callbacks, adds no queue, and has no new reentry path.
Existing UDP callbacks retain their own owner/reentry/settlement contract.

`tests/contract/udp/test_udp_peer.cpp` directly verifies interface identity,
thread rejection, send admission, close/requestClose and outliving-loop behavior.
`tests/ci/run_udp_contracts.py` executes ON adapter consumers, UDP-only/Core-only
discovery and physical OFF rejection on Windows/Linux and sanitizers.
The scope guard must continue rejecting UDP-to-transport and reverse includes;
only this composition component may include both. Stable Core/UdpSocket and
TransportEndpoint APIs remain unchanged.
