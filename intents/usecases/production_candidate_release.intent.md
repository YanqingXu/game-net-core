---
status: active
target: GameNet::core
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
---

# Use-Case Intent: Production Candidate Release

## 1. Intent
Phase 6 turns the hardened networking foundation into an auditable production
candidate. A candidate is eligible for release only when its supported public
surface, observability, performance envelope, endurance behavior, platform
matrix, and immutable evidence are explicit and machine checked.

## 2. Responsibilities
- publish a versioned inventory of installed targets and public headers
- distinguish stable Core source contracts from provisional upper-layer and
  unsupported platform-backend headers
- reject unreviewed drift in the declared stable Core surface
- expose bounded, non-owning production metrics through a promoted exporter
  contract
- compare fixed benchmark scenarios against same-platform baselines with
  explicit regression budgets
- retain structured 24-hour candidate and 72-hour release endurance evidence
- exercise recoverable network, resource-pressure, callback, and shutdown
  failures without weakening lifecycle or thread-affinity contracts
- bind every release claim to one immutable candidate commit and a declared
  Linux/Windows support matrix

## 3. Non-Responsibilities
- no ABI compatibility guarantee before version 1.0
- no stability promise for provisional upper-layer or platform-backend headers
- no HTTP, RPC, TLS, UDP, KCP, coroutine, or business-logic expansion
- no cross-host comparison of raw benchmark scores
- no release claim based only on a local run, shortened endurance run, or
  documentation assertion

## 4. Compatibility Contract
- `api/public_api_manifest.json` is the authoritative installed-surface
  classification for the candidate line
- stable Core declarations require an intentional manifest update and review;
  removals or incompatible changes require a documented migration decision
- ABI remains explicitly unsupported until a later intent defines toolchain,
  standard-library, compiler-option, and symbol-version boundaries
- provisional and platform-internal categories must remain visibly distinct
  from the stable Core promise

## 5. Threading and Ownership Rules
- compatibility verification is a build-time repository guard and owns no
  runtime state
- metrics recorders may be called from producer loops but must not transfer
  producer ownership or block those loops on exporter I/O
- endurance and fault drivers own their client sockets and marshal every
  server mutation through the existing owner-loop APIs
- release automation owns evidence files only; it does not own runtime network
  objects or alter callback affinity
- this intent adds no re-entrant runtime callback before the metrics exporter
  contract is promoted and tested

## 6. Candidate Exit Gates
1. compatibility manifest and guard pass in every ordinary CI producer
2. production metrics exporter contracts pass under concurrency and teardown
3. fixed Release benchmarks stay within reviewed same-runner regression budgets
4. fault-injection contracts pass on Linux and Windows
5. one frozen commit completes the 24-hour candidate endurance gate
6. the same frozen commit completes the 72-hour release endurance gate
7. all supported platform, sanitizer, package, benchmark, and evidence gates
   are green and retained

## 7. Verification
- `tests/api/test_public_api_manifest.py` verifies the installed inventory,
  stability classes, package version, exported targets, and stable-header
  fingerprints and proves that missing or modified declarations are rejected
- `tests/ci/test_performance_regression.py` verifies the fixed 12-scenario,
  three-repetition baseline/candidate matrix, reviewed budgets, same-runner
  workflow wiring, retained evidence, and a real failing-regression fixture

## 8. Review Checklist
- Is every release claim backed by structured same-commit evidence?
- Did a stable declaration change without an explicit compatibility decision?
- Are platform and compiler support boundaries stated precisely?
- Can any metrics, fault, or endurance path block or mutate a non-owner loop?
- Are 24-hour and 72-hour results real durations rather than scaled substitutes?
