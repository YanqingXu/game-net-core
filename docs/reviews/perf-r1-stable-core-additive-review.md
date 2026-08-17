# PERF-R1 Stable Core Additive Review

Status: `approved-additive-source-compatible`.

This focused review accepts one deliberate stable Core addition needed to make
the fixed capacity profile deterministic on Linux/epoll and Windows/IOCP. It is
an implementation-author review under the repository's same-line compatibility
procedure; it is not presented as a second independent API-R1 review. The
original API-R1 independent decision and its historical snapshot remain intact.

## Identity

```text
prior reviewed surface: api-r1-approved-surface
prior reviewed commit: 9d2a5be0eb5439399f27c2f53ec1bf985c7de1d0
reviewed implementation tag: api-r1-perf-r1-reviewed-surface
reviewed implementation commit: 6b292156e3e94d3389e9f3b8513445e7eb4ab541
reviewed snapshot: api/baselines/v0.3.0-perf-r1-reviewed.json
candidate tag: v0.3.0-rel-c1-refreeze-2
review date: 2026-08-17 (Asia/Shanghai)
```

The same-line diff from the prior reviewed surface contains no header or target
addition/removal/category move and exactly one stable fingerprint change:
`include/gamenet/core/net/TcpConnection.h`. The new reviewed snapshot is a zero
diff against the implementation checkpoint. Both structured results are
archived beside this record.

## Contract decision

`TcpConnection::setSendBufferSize(std::size_t)` is accepted as an additive,
source-compatible configuration method:

- no existing declaration, constructor, default, enum value, or exact overload
  is removed or changed;
- the method is owner-loop-only, matching all other connection socket-option
  mutation;
- zero and values outside the positive native `int` range are rejected;
- `setsockopt(SO_SNDBUF)` failure is reported as `std::system_error`;
- the operating system may round the requested size, so the API promises a
  request rather than an exact effective size;
- kernel send-buffer configuration does not replace or weaken connection,
  loop, server, or global application output admission;
- ownership, callback ordering, establishment, close, and completion-drain
  state machines do not change.

ABI remains explicitly unsupported before 1.0. Existing 0.3 source consumers
do not need a migration; all consumers continue to rebuild for each release and
toolchain combination.

## Intent, affinity, and ownership review

The TcpConnection and production-candidate intents authorize the narrow
configuration path. `rules/thread_affinity_rules.md` requires invocation on the
connection owner loop. The capacity harness calls it from the established
callback before application sends. No cross-thread socket access, new callback,
new owner, or retained resource is introduced. The server still owns the
connection; TcpConnection still owns the socket and closes it through the
existing lifecycle.

## Direct evidence

- `tests/contract/tcp_connection/test_tcp_connection_socket_options.cpp`
  checks effective native bounds after a positive request, zero and oversized
  rejection, and wrong-thread rejection on Windows and Linux.
- `tests/cmake/test_tcp_connection_thread_contract.py` requires the explicit
  owner-loop assertion and the direct contract in the threading/lifecycle
  slices.
- `tests/cmake/test_capacity_profile_contract.py` freezes
  `server_send_buffer_bytes=4096`, validates its native range, preserves legacy
  documents that omitted the field, and requires exact paired profile identity.
- `tests/api/test_public_api_manifest.py` verifies the tagged snapshot against
  the Git tree, archives the one-fingerprint decision diff, and requires a zero
  diff from the new reviewed surface.

The full local `candidate-10k` preflight passed three repetitions per platform:
Windows produced 8,252 accepted plus 1,748 typed overload results per sample;
WSL Linux produced 8,000 plus 2,000. All six samples completed 500/500 healthy
probes, recovered 1,000/1,000 sockets through 16 readers, converged pending
output to zero, and passed the v3 validator. Those runs are feasibility
evidence only; immutable candidate workflow evidence must be regenerated.

## Decision

Approve the additive source contract and bind it to
`api-r1-perf-r1-reviewed-surface@6b292156e3e94d3389e9f3b8513445e7eb4ab541`.
Any subsequent stable header or target drift remains blocked by the zero-diff
compatibility gate. This review does not approve release, performance evidence,
endurance evidence, licensing, or the candidate tag by itself.
