# API Compatibility Policy

`api/public_api_manifest.json` is the v2 machine-readable compatibility
boundary for the 0.3 production-candidate line.
The frozen line installs as package version `0.3.0`; its CMake project version,
manifest package version, explicit `compatibility_line`, exact-version
consumer, and release label must agree.

The manifest classifies both exported CMake targets and installed public
headers. It also binds the candidate to the reconstructed
`v0.2.0-phase4-preview` snapshot by tag, full commit, repository-relative
snapshot path, and snapshot SHA-256. The hash uses UTF-8 content with normalized
LF line endings, so Windows and Linux checkouts verify the same snapshot
identity. All CI producers fetch full history: the guard also peels the tag,
enumerates its public headers/exported targets, verifies its package version,
and recomputes every frozen stable-header fingerprint from that Git object.
This prevents a change from replacing both the snapshot and its declared hash
with content that never existed at the release tag.

## Compatibility Classes

- `stable_core`: `GameNet::core` and its supported Core headers. Source
  compatibility is required within the 0.3 line. Any
  declaration-level drift must be reviewed and recorded by updating the
  manifest fingerprint. Removing or incompatibly changing a declaration also
  requires a migration decision in the release notes.
- `provisional`: the exported Phase 4 protocol, transport, session, logic, and
  broadcast targets and headers plus the initial metrics exporter/recorder
  headers remain available for evaluation but may change before 1.0. Metrics
  stay provisional until their allocation, contention, type, and
  enabled-overhead contracts are promoted.
- `platform_internal`: backend and operating-system integration headers are
  installed today for implementation reasons but are unsupported application
  interfaces. There is currently no separately exported internal target.

The project does not guarantee ABI compatibility before 1.0. Compiler, C++
standard-library, build-option, and runtime combinations are therefore not
interchangeable binary contracts. Consumers should rebuild against each 0.x
release.

## Change Procedure

Run the verifier and emit the deterministic historical diff:

```text
python tests/api/test_public_api_manifest.py
python tools/compare_public_api_manifest.py --output public-api-diff.json
```

The guard rejects missing, newly unclassified, or multiply classified public
headers or targets; package-version and compatibility-line drift; a stale or
tampered or tag-inconsistent historical snapshot; and any unrecorded stable
Core declaration change. Comments and whitespace are excluded from the stable
fingerprints.

The diff records target and header additions, removals, category moves, and
stable-header fingerprint changes in sorted JSON. Its summary separates two
questions:

- `stable_surface_review_required` is true for any change touching the stable
  surface, including an additive promotion.
- `compatibility_decision_required` is true when a removal, downgrade, or
  stable fingerprint change occurs inside the same compatibility line.

Cross-line changes such as the current 0.2 preview to 0.3 candidate comparison
still require review, but they do not claim to violate the 0.3-within-line
promise. The raw changes remain present in the evidence even when the decision
flag is false.

A deliberate stable API change must update the public contract, direct tests,
manifest fingerprint, and release notes in one reviewed change. Updating a
fingerprint alone is not evidence that the change is compatible.

## Current M1 Additive Review

The 2026-07-27 M1 lifecycle closure adds `TcpClientControl.h` and
`TcpConnectionClose.h` to `stable_core`. It also updates the recorded
declaration fingerprints for EventLoop/Poller/Socket, TCP callbacks,
TcpClient/TcpConnection/TcpServer, and callback-exception source reporting.
Direct lifecycle, saturation, completion-drain, close-reason, detached-handle,
and install-consumer contracts cover the new surface.

The deterministic comparison against the 0.2 Preview reports a stable-surface
review requirement and no within-line compatibility-decision requirement.
That result records an additive 0.3-candidate surface; it does not replace the
independent maintainer review required before freezing or tagging a release.

The primary Linux CI producer writes
`ci-evidence/public-api-diff.json` before its evidence manifest is built. The
existing per-job evidence artifact therefore retains the diff for 90 days and
the aggregate evidence gate covers the producer artifact.

## Current M3-G5 Additive Review

M3-G5 adds `Acceptor::setIocpAcceptDepth()` /
`Acceptor::iocpAcceptDepth()` and `TcpServer::setIocpAcceptDepth()` to the
stable Core source surface. Existing constructors and defaults remain source
compatible; applications that do not configure the option receive the bounded
Windows default depth of four, while non-Windows readiness behavior is
unchanged. Reconfiguration is rejected after listen/start, and `[1, 64]`
validation prevents an unbounded pool.

The `Channel.h` fingerprint also changes because its private Windows backend
state now carries the exact IOCP operation identity published for the current
active entry plus an allocation-free head/tail pair for bounded Accept
completion coalescing. This is not an application scheduling API and adds no
public method. No binary ABI promise exists before 1.0, so consumers rebuild
as already required.

Direct contracts are
`tests/contract/acceptor/test_acceptor_contract.cpp`,
`tests/contract/acceptor/test_acceptor_iocp_pool.cpp`,
`tests/contract/tcp_server/test_tcp_server_contract.cpp`, and
`tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`.
They cover the public option, finite default/configured depth, exact completion
identity, burst replenishment, Retry/Stop cancellation, synchronous failure,
callback re-entry, and final-drain convergence.
