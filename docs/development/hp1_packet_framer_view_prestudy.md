# HP1 PacketFramer Borrowed-View Prestudy

Date: 2026-08-25

Decision: `KEEP-EXPERIMENTAL`

Evidence class: development-only, non-promotion

## Scope And Boundary

The prestudy implements `PacketView`, move-only `OwnedPacket`,
`FrameVisitResult`, and `PacketFramerViewPrototype::visitFrames` only inside the
default-off `GAMENET_BUILD_BENCHMARKS=ON` graph. Nothing is installed, exported,
linked into the production Core path, added to the public manifest, or selected
by the current Profile A example.

HP0 fixed-lab evidence remains `DEFER`. This note neither closes HP1 nor
authorizes `PacketFramer::visitFrames` as an installed API.

## Lifecycle Answers

| Question | Prestudy answer |
| --- | --- |
| Owner thread | The connection EventLoop that owns the source Buffer owns construction, visit, consumption, reset, and destruction. |
| Ownership/release | Buffer owns readable storage; PacketView borrows only during one visitor; retain copies once into move-only OwnedPacket; the prototype retains no payload. |
| Callback re-entry | Higher-level owner-only stop/close is allowed; source-Buffer mutation, nested visit, and reset are forbidden, with nested visit/reset explicitly rejected. |
| Cross-thread operation | PacketView cannot cross threads or suspension. A later typed bounded mechanism may move OwnedPacket; HP1 creates no mailbox. |
| Shutdown settlement | The prototype owns no queued work or callbacks. Protocol/visitor failure is terminal for integration, and destruction has zero retained view/packet state. |

## Correctness Evidence

Windows MSVC Release, benchmark-enabled build:

```text
contract.protocol.test_packet_framer_contract ........... Passed
contract.protocol.test_packet_framer_budget ............. Passed
contract.protocol.test_packet_framer_round_trip_smoke ... Passed
contract.protocol.test_packet_framer_view ............... Passed
4/4 passed
```

The new contract covers partial/sticky/empty frames, frame and byte budgets,
exact Buffer consumption, oversized-prefix preflight before callbacks, sticky
fault/reset, visitor exception settlement, nested visit/reset rejection,
explicit retain, zero-allocation owner-local visit, and 200-frame deterministic
randomized differential decoding against legacy `PacketFramer::push()`.

The focused MSVC Debug AddressSanitizer build passed
`contract.protocol.test_packet_framer_view` 1/1.

Intent metadata/semantics/consistency, HP0 benchmark governance, migration
status, scope, and public API guards also passed. The public API manifest has no
change.

## Development Benchmark

Each row is ten sequential Release samples from the current dirty development
worktree. There was no fixed-runner observer, unrecorded warmup, raw artifact
retention, confidence interval, or cross-platform pairing; therefore the table
is not promotion evidence.

| Payload bytes | Iterations × frames | Legacy median ns | Candidate median ns | Elapsed improvement | Legacy allocations/sample | Candidate allocations/sample |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 20,000 × 32 | 71,763,750 | 17,960,650 | 74.97% | 880,000 | 0 |
| 256 | 10,000 × 32 | 93,108,750 | 63,326,300 | 31.99% | 440,000 | 0 |
| 4,096 | 1,000 × 32 | 156,621,300 | 99,481,450 | 36.48% | 44,000 | 0 |
| 16,384 | 250 × 32 | 203,123,600 | 99,650,500 | 50.94% | 11,000 | 0 |

Every sample reported schema
`gamenet.hp1_packet_framer_view_prestudy.v1`, `status=ok`,
`evidence_class=development`, `promotion_eligible=false`, equal checksums and
frame counts, zero candidate allocations, zero candidate copied bytes, and
zero retained bytes.

## Missing Evidence And Decision

The following remain missing:

- clean exact-commit native Linux and Windows HP0 ledgers;
- Linux correctness/sanitizer execution and candidate libFuzzer coverage;
- real Profile A `TcpConnection::inputBuffer` integration and its full
  re-entry/shutdown suite;
- fixed observer cycles/instructions/LLC/syscall data and 95% bootstrap result;
- installed API review and stable/provisional manifest decision.

The structural contract and development signal justify retaining the isolated
prototype, so the prestudy decision is `KEEP-EXPERIMENTAL`. The implementation
slot is released for the next HP prestudy. Formal HP1 remains `planned` until
HP0 closes and the candidate is replayed through the normal slice.
