# M10 UDP / KCP Experimental Promotion Audit

Date: 2026-08-24

Core baseline: `f1f89f0e66642b4be3c988400213783e5120536c`

Historical migration source:
`mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`

## Decision

- shared UDP/KCP/PMTU promotion: `NO-PROMOTION`;
- `udp`, `kcp_transport`, `path_mtu_cache`,
  `platform_path_mtu_signal`, and `path_mtu_signal_authentication` remain
  `deferred`;
- active `PlayerSession` / `SessionManager` remains a single-current-endpoint
  contract and is not extended into an automatic TCP/UDP dual channel;
- no Core UDP/KCP header, experimental datagram target, package component,
  compatibility promise, tag, or empty v0.9 release is created;
- M10 is closed and the unique governance front advances to M11 v1.0
  stabilization and release readiness.

The first two M10 startup conditions are historical facts, but the mandatory
intent-promotion and lifecycle-review conditions fail. There are zero real
external UDP/KCP consumers and the preserved source design does not meet the
current bounded, typed, generation-safe, dual-platform contract.

## Startup-Gate Audit

| Gate | Evidence | Result |
| --- | --- | --- |
| v0.3 external release | Stable Apache-2.0 `v0.3.0@8e4a6ed` is published with its recorded release matrix | Satisfied |
| two TCP Runtime Models validated by M3 | The real gateway exercised Queued Event and Sharded Hybrid TCP profiles through the installed package | Satisfied for TCP only |
| UDP/KCP/PMTU intents formally promoted | All five formal module intents still say `status: deferred`; the broad game-network scope text is `legacy` | Failed |
| owner/session-generation/MTU/retransmission/backpressure/dual-close review | No active intent or two-consumer contract defines the required bounded datagram lifecycle | Failed |

The failed mandatory gates stop DGM-U1 before implementation.

## Audited Inputs

### Current GameNet Core

Core has no UDP socket/server, KCP codec/session/transport, PMTU cache/signal,
or installed `GameNet::experimental_datagram` surface. Platform socket helpers
recognize `EMSGSIZE` only as a generic socket error fact; that is not a
datagram-transport contract.

The reusable active session boundary remains transport-neutral and singular:

- `SessionManager` owns one current `SessionBinding(sessionId, transportId,
  generation)` per player on its management EventLoop;
- replacement revokes the prior binding generation before publishing a new
  endpoint, and stale work cannot send or mutate through the replacement;
- endpoint closure marshals to the endpoint owner through the existing typed
  lifecycle lane;
- it does not bind simultaneous mutable TCP and UDP owners or infer a
  dual-channel close policy.

The directly relevant SessionManager contract, lifecycle, and dispatch tests
passed 3/3. They protect the current generation-safe single-endpoint behavior;
they are not UDP/KCP evidence.

### `gamenet-game-gateway`

The exact gateway closure
`588acd079be93de3e230ba4f07dd111f7bec6a3c` has no tracked UDP, KCP, PMTU,
datagram, or experimental-datagram implementation. M3/M7 intentionally stayed
on the official TCP package path and explicitly excluded UDP/KCP.

### `YanGameServer`

The independent checkpoint
`b5254165389d762c3f3c63568c24ffab448fc501` likewise has no runtime or test
path containing `UdpSocket`, `UdpServer`, `KcpTransport`, `KcpSession`, PMTU,
or an experimental datagram component. Its independent native transport is
TCP-only internal RPC.

Neither external project supplies a first consumer, so no two-consumer
promotion comparison exists.

### Historical `mini_trantor`

The clean nested migration source at
`3eba368475a68f677aae920d4f299b155db23d57` contains UDP/KCP/PMTU source and
tests, but it is source provenance, not an independent current consumer.

Its public shape also predates current Core gates:

- UDP/KCP `start`, `stop`, `sendTo`, and close methods return `void`; rejected
  admission, owner shutdown, stale session, queue pressure, and `EMSGSIZE` are
  not expressed as current typed results;
- `UdpServer` maps peer address directly to a monotonically assigned session id
  but exposes no generation-bearing peer/session token;
- `UdpTransportEndpoint` silently returns when its weak lifetime expires and
  exposes the old mutable/context-heavy transport interface instead of the
  active typed `TransportEndpoint` contract;
- KCP keeps `sendQueue`, `inFlight`, `pendingPackets`, fragment assemblies,
  sessions, parity history, and MTU cache containers without configured total
  count/byte/per-turn admission bounds;
- `PathMtuCache` explicitly defers bounded capacity and eviction;
- raw ICMP, signal authentication, redundant copies, and XOR parity are mixed
  into the same candidate even though the current M10/v1 plan leaves raw ICMP,
  authenticated PMTU, and FEC after v1.

The old Windows test graph returns early after registering only the KCP codec;
it registers no UDP loopback, shutdown, or KCP transport contract. A fresh
Windows Release attempt with TLS disabled failed before producing the codec
test because an unrelated WebSocket source unconditionally required
`openssl/evp.h`. A fresh WSL Linux configure with TLS enabled failed because
OpenSSL development files were unavailable. These failed setup attempts are
recorded only as missing portability/build evidence, never as product test
failures or passing contracts. No historical mini-trantor runtime result is
claimed by M10.

## DGM Slice Decision

| Planned slice | Missing evidence | Decision |
| --- | --- | --- |
| DGM-U1 UDP owner-loop foundation | No active intent, typed send/receive terminal, generation token, Windows loopback/shutdown, or real consumer | `NO-PROMOTION` |
| DGM-U2 Datagram TransportEndpoint | Old endpoint silently drops and does not implement current typed owner/lifetime contract; no dual-channel owner policy | `NO-PROMOTION` |
| DGM-K1 reliable datagram | No bounded queue/window/fragment/in-flight/per-turn contract or deterministic current test execution | `NO-PROMOTION` |
| DGM-K2 lifecycle/pressure | No handshake timeout, stale generation, MTU reduction, slow-peer, capacity, soak, or dual-platform evidence against current Core | `NO-PROMOTION` |
| DGM-X1 experimental install | No verified implementation closure to install and no independent experimental API manifest/package consumer | `skipped-by-evidence` |

## Deferred Intent Review

Before any future resume:

1. `udp` must drop raw-ICMP scope from the initial slice and define typed
   datagram admission/terminal results, copied payload ownership, peer
   generation, exact callback re-entry, Windows completion/readiness behavior,
   and bounded receive/send work.
2. `kcp_transport` must be reduced to the DGM-K1/K2 scope and add hard
   count/byte/per-turn limits for every retained container, timer/retry
   retirement, stale packet generation, slow-peer policy, and exact shutdown
   convergence. FEC, redundant-copy, and raw-ICMP work cannot enter by accident.
3. `path_mtu_cache` needs bounded capacity/eviction and a clear path-identity
   model before it can be shared across owners.
4. `platform_path_mtu_signal` needs honest Linux/Windows capability and runtime
   tests; unsupported capability must remain explicit.
5. `path_mtu_signal_authentication` remains post-v1 because it is not
   cryptographic authentication and the current plan excludes authenticated
   PMTU/raw ICMP from v1.

Promotion still requires two real consumers and direct Windows/Linux contract,
sanitizer/race, deterministic impairment, capacity/soak/benchmark, manifest,
and installed-package evidence.

## Ownership, Re-entry, and Cross-Thread Decision

This audit creates no UDP socket, timer, session owner, callback, or
cross-thread path. Existing Core EventLoop, SessionManager, and endpoint owners
remain unchanged. Callback re-entry and terminal work continue through current
typed bounded facilities; no UDP/KCP work is silently queued or dropped by a
new adapter.

## Repository Verification

`tests/cmake/test_migration_status_contract.py` binds this audit, the five
deferred intent/catalog entries, exact Core/external/source checkpoints, the
3/3 current session evidence, both failed historical-source setup attempts,
the per-slice decisions, M10 `NO-PROMOTION`, M11 as the next governance front,
and the absence of installed UDP/KCP/PMTU/datagram headers or CMake targets.
