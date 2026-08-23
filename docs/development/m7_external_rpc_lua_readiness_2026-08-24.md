# M7 External Lua / Typed-RPC Readiness Audit

Date: 2026-08-24

Core baseline: `66e7389cf6a2f52b3f13c85ffdbe1dff6a90cf50`

Exact readiness-audit checkpoint:
`44493b1d37c16567990e1660153d6b0843a8eecc`.

Initial disposition at `44493b1d37c16567990e1660153d6b0843a8eecc`:

- external implementation: `DEFER`;
- shared GameNet RPC promotion: `NO-PROMOTION`;
- `intents/modules/rpc.intent.md`: remains `deferred`;
- M7 remained the governance front until an evidence-eligible gateway slice
  became available.

Resume recheck after gateway M4 closure:

- external implementation: `RESUME` at gateway governance checkpoint
  `92a26072c3300275edc9d069a59fc17913c7614c`;
- shared GameNet RPC promotion: still `NO-PROMOTION`;
- `intents/modules/rpc.intent.md`: remains `deferred`;
- M7 remains the governance front while the external callback/value adapter is
  implemented and compared with the independent consumer.

Final closure after external implementation and comparison:

- external adapter validation: `COMPLETE` at gateway implementation checkpoint
  `e43393c85fa37604d340fe866610756c99f4fe4e`;
- gateway M7 closure: `NO-PROMOTION` at
  `588acd079be93de3e230ba4f07dd111f7bec6a3c`;
- shared GameNet RPC promotion: `NO-PROMOTION`;
- `intents/modules/rpc.intent.md`: remains `deferred`;
- M7 is closed and the governance front advances to M8 evidence review without
  adding a Core RPC/Lua surface.

The initial and resume sections are readiness history. The final section binds
the external implementation result and comparison decision; none of these
decisions adds a Core RPC/Lua target, header, package component, wire format,
executor, tag, or GitHub Release.

## Audited Consumers

### `gamenet-game-gateway`

The historical M3 gateway evidence is
`0a8fe1e43cb11ac32daa8f9266d3b84924736e67`. Its installed-Core TCP path has
one owner-isolated, bounded Lua execution-cell seam and verifies blocking,
exception containment, typed saturation, callback kick re-entry, generation
revalidation, and deterministic shutdown in:

- `tests/integration/test_queued_event_auth_session.cpp`;
- `tests/integration/test_queued_event_output_shutdown.cpp`;
- `tests/integration/test_sharded_hybrid_gateway.cpp`;
- `tests/integration/test_gateway_replay_fault_endurance.cpp`.

That seam is an injected callback, not a Lua VM adapter. At the initial audit,
the committed M3 intent excluded RPC and a user-owned M4 package-adoption plan
was still uncommitted, so the audit preserved the checkout and recorded
`DEFER`.

Gateway M4 subsequently closed as official-package `ADOPTED` at clean commit
`d03cacd5aead885fc61a419c71d5c32a060cb700`. The separately approved gateway M7
intent/rules/contract names are committed at
`92a26072c3300275edc9d069a59fc17913c7614c`. They authorize only an external,
bounded, non-coroutine callback/value adapter and preserve the installed-Core
boundary. This satisfies resume gates 1 and 2 without creating RPC evidence or
authorizing shared promotion.

The gateway then implemented the governed slice at clean commit
`e43393c85fa37604d340fe866610756c99f4fe4e` and closed it at
`588acd079be93de3e230ba4f07dd111f7bec6a3c`. Package-only Windows/IOCP and
Linux/epoll repositories passed 11/11, Linux ASan/UBSan passed 11/11, the two
focused RPC tests passed 20/20 each on both platforms, and the codec fuzz target
passed 100,000 Clang ASan/UBSan runs. The true-TCP adapter proved the named
partial-frame, saturation/recovery, terminal-race, callback-re-entry, and zero-
obligation contracts without a Core source dependency.

### `YanGameServer`

The independent clean consumer checkpoint is
`b5254165389d762c3f3c63568c24ffab448fc501`. It pins the stable GameNet Core
source revision `8e4a6edfe22ca43e3308e36ec31bf7f2dea14ac7` for its GameNet transport
adapter and separately implements an owner-confined, bounded RPC/session layer,
a native-socket remote adapter, and an owner-cell script runtime.

The exact checkpoint was read-only archived away from its dirty current checkout,
rebuilt in Windows Release contract-fallback configuration, and the
following focused callback/value, lifecycle, integration, and stress contracts
passed 8/8:

- `yangame-rpc-envelope`;
- `yangame-rpc-wire-codec`;
- `yangame-rpc-dispatch`;
- `yangame-rpc-session-table`;
- `yangame-rpc-pending-stress`;
- `yangame-lua-rpc-await`;
- `yangame-actor-rpc-flow`;
- `yangame-remote-rpc-transport`.

This focused local configuration used the dependency-fallback script/TLS
adapters and fetched the exact stable GameNet source pin; it is not claimed as
real-Luax, real-mTLS, installed-package, Linux, sanitizer, or endurance
evidence. The committed YanGame checkpoint separately records its governed
Luax RC pin and upstream CI evidence, but that evidence does not create a
shared GameNet RPC contract.

## Contract Comparison

| Boundary | Gateway checkpoint | YanGameServer checkpoint | M7 conclusion |
| --- | --- | --- | --- |
| Lua ownership | Distinct bounded Lua-cell owner; network callbacks only submit copied values | Execution-cell/shard owner retains VM and continuation state | Same principle, different mailbox/runtime contract |
| Wire | `GRPC` v1 request/response/error; generation/request plus one service/method; arbitrary bounded bytes | `YGRP` v2 message/failure; session/request, source/target node/service, message/schema, Actor route, runtime causality, allowlisted business payload | Magic, version, record, identity, routing, causality, and payload fields differ |
| Deadline | Remaining milliseconds, reduced by Lua queue time | Remaining microseconds with mandatory hop charge | Same no-foreign-clock principle, incompatible wire semantics |
| Transport | Installed GameNet TCP/PacketFramer; per-connection EventLoop channel | Private endpoint/server workers with TLS 1.3 mutual authentication and node handshake | Owner and handshake contracts differ |
| Request state | Per-connection count/byte/history bounds; monotonic non-reused ID per generation; callback/value completion | Construction-thread session table with bounded probing/reuse, waiters/coroutine bridge, and Actor-supplied timer | Bounds overlap, but correlation, owner, and completion APIs differ |
| Terminal behavior | Response/remote-error/timeout/cancel/close/overload/shutdown/owner-unavailable | Response/failure/timeout/cancel/disconnect/shutdown with retained ErrorCode | Exactly-once is shared conceptually, not a substitutable lifecycle API |

The old deferred GameNet RPC intent also describes a different string-method,
per-connection protocol and mixes callback and coroutine scope. It is a design
asset only and cannot be activated by renaming YanGame concepts or copying its
wire format.

## Ownership and Threading Decision

This audit creates no runtime owner. For a future gateway slice:

- the connection EventLoop owner retains framing, transport send/close, and
  per-connection RPC channel mutation;
- the gateway logic/Lua cell owner retains VM state and business handler work;
- pending requests own copied request/callback values only until one terminal
  response, error, timeout, disconnect, or shutdown result;
- close, timeout, handler completion, and callback re-entry must revalidate the
  exact connection/request generation before further work;
- cross-owner work uses existing typed bounded admission and immutable/moved
  values; network callbacks never enter the VM and saturation never falls back
  to inline execution.

The concrete future gateway test file must be named before implementation and
must cover malformed, oversized, and partial frames; duplicate, unknown, and
late responses; pending-count/byte saturation and recovery; timeout/response/
close/shutdown races; handler exceptions; callback-reentrant close/stop; dual-
platform real TCP; fuzzing; and zero retained pending/callback state.

## Resume Gates

External M7 implementation closed these gates:

1. **satisfied** — gateway M4 is closed cleanly at `d03cacd5aead885fc61a419c71d5c32a060cb700`;
2. **satisfied** — gateway M7 governance at `92a26072c3300275edc9d069a59fc17913c7614c`
   explicitly authorizes callback/value RPC after the historical M3 boundary;
3. **satisfied** — gateway implementation
   `e43393c85fa37604d340fe866610756c99f4fe4e` verifies the bounded
   non-coroutine adapter and Lua-cell integration described above;
4. **satisfied for comparison input** — the independent clean checkpoint
   remains `b5254165389d762c3f3c63568c24ffab448fc501` with focused 8/8 evidence;
5. **executed, divergent** — the field-by-field comparison proves different
   wire, correlation, owner, payload, handshake, and completion contracts.

The final gate diverged, so M7 closes as `NO-PROMOTION` and both adapters remain
external. `intents/modules/rpc.intent.md` is not rewritten or promoted.

## Repository Verification

`tests/cmake/test_migration_status_contract.py` verifies this audit, the exact
external implementation/closure/checkpoint decisions, the deferred RPC intent/
catalog state, and the absence of installed GameNet RPC/Lua headers or CMake
targets.
