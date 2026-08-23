# M7 External Lua / Typed-RPC Readiness Audit

Date: 2026-08-24

Core baseline: `66e7389cf6a2f52b3f13c85ffdbe1dff6a90cf50`

Disposition:

- external implementation: `DEFER`;
- shared GameNet RPC promotion: `NO-PROMOTION`;
- `intents/modules/rpc.intent.md`: remains `deferred`;
- M7 remains the governance front until an evidence-eligible gateway slice is
  available or the owner explicitly closes the milestone as `NO-PROMOTION`.

This is a readiness decision, not an implementation or release result. It adds
no RPC/Lua target, header, package component, wire format, executor, tag, or
GitHub Release.

## Audited Consumers

### `gamenet-game-gateway`

The last clean committed gateway evidence is
`0a8fe1e43cb11ac32daa8f9266d3b84924736e67`. Its installed-Core TCP path has
one owner-isolated, bounded Lua execution-cell seam and verifies blocking,
exception containment, typed saturation, callback kick re-entry, generation
revalidation, and deterministic shutdown in:

- `tests/integration/test_queued_event_auth_session.cpp`;
- `tests/integration/test_queued_event_output_shutdown.cpp`;
- `tests/integration/test_sharded_hybrid_gateway.cpp`;
- `tests/integration/test_gateway_replay_fault_endurance.cpp`.

That seam is an injected callback, not a Lua VM adapter. The committed M3
intent and repository instructions explicitly exclude RPC. The checkout also
contained a user-owned, uncommitted `plan.md` change for a separate v0.3.0
package-adoption track. It was preserved and is not exact-commit evidence.
Consequently the audit neither edits the gateway nor treats it as an RPC
consumer.

### `YanGameServer`

The independent clean consumer checkpoint is
`b5254165389d762c3f3c63568c24ffab448fc501`. It pins the stable GameNet Core
source revision `8e4a6edfe22ca43e3308e36ec31bf7f2dea14ac7` for its GameNet transport
adapter and separately implements an owner-confined, bounded RPC/session layer,
a native-socket remote adapter, and an owner-cell script runtime.

The current checkpoint was rebuilt in Windows Release configuration and the
following focused callback/value, lifecycle, integration, and stress contracts
passed 8/8:

- `yangame-rpc-envelope`;
- `yangame-rpc-wire-codec`;
- `yangame-rpc-dispatch`;
- `yangame-rpc-session-table`;
- `yangame-rpc-pending-stress`;
- `yangame-integration`;
- `yangame-lua-rpc-await`;
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
| Lua ownership | One logic/cell owner; network callbacks never execute the injected Lua seam | One execution-cell/shard owner; VM entry and retained continuation state remain owner-confined | Same ownership principle, but no shared installable runtime abstraction is demonstrated |
| Wire | No RPC frame | Internal wire v2 with magic/version, numeric service/method/message identities, runtime causation/correlation, and bounded typed payloads | No common wire exists |
| Transport | Installed GameNet TCP path | RPC remote adapter privately uses native sockets and requires its own TLS/node handshake contract | No shared per-GameNet-connection channel exists |
| Request state | No RPC pending table | Owner-confined count/byte-bounded dispatcher and pending/history tables | Gateway has not validated the same lifecycle |
| Terminal behavior | No RPC response/timeout/late-response contract | Exactly-once response/error/timeout/cancel/disconnect/shutdown settlement | Cannot promote from one consumer |
| Coroutine dependency | Gateway Lua seam is callback/value based | Core RPC values are callback/completion-capable, with a separate Task bridge | A future gateway slice must remain valid without coroutine support |

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

External M7 implementation remains deferred until all of the following are
true:

1. the gateway has one clean exact commit and no conflicting active adoption
   task or user-owned worktree change;
2. a gateway M7 intent/rule update explicitly authorizes callback/value RPC
   after the M3 no-RPC boundary;
3. the gateway implements and verifies the bounded non-coroutine adapter and
   Lua-cell integration described above;
4. a second independent consumer is revalidated at one clean exact commit;
5. a field-by-field comparison proves the same wire and lifecycle need.

Only then may `intents/modules/rpc.intent.md` be rewritten against current code,
promoted to active, and mapped to concrete `GameNet::protocol` tests. If the
wire/lifecycle comparison still diverges, M7 closes as `NO-PROMOTION` and both
adapters remain external.

## Repository Verification

`tests/cmake/test_migration_status_contract.py` verifies this audit, the exact
external checkpoints and decisions, the deferred RPC intent/catalog state, and
the absence of installed GameNet RPC/Lua headers or CMake targets.
