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
- bind the current manifest to an immutable historical tag, commit, snapshot
  path, and content hash; verify the snapshot inventory and stable fingerprints
  against that Git object; then emit deterministic target/header category diffs
- reject unreviewed drift in the declared stable Core surface
- expose bounded, non-owning observability through a provisional exporter
  contract without freezing the current hot-path API
- compare fixed benchmark scenarios against same-platform baselines with
  explicit regression budgets
- retain paired Linux/Windows 10k mixed slow-reader/Broadcast capacity evidence
  with exact healthy-probe and bounded recovery-reader lifecycle accounting;
  freeze the same finite server send-buffer request on both platforms so typed
  application overload does not depend on each host's kernel default
- keep the 100k endpoint-attempt profile on explicitly labeled dedicated
  capacity runners; it is not an ordinary hosted-CI or cross-host score gate
- retain structured 24-hour candidate and 72-hour release endurance evidence
- emit one promotion manifest that revalidates the retained raw capacity and
  endurance inputs, binds their exact workflow run/attempt identities to the
  same immutable commit, and distinguishes candidate from release requirements
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
- the frozen candidate declares package version `0.3.0`; the CMake package,
  public API manifest, exact-version consumer, and candidate baseline label
  must agree before any long-duration evidence is eligible
- `api/public_api_manifest.json` is the authoritative installed-surface
  classification for the candidate line
- the manifest declares its compatibility line and historical snapshot; CI
  retains the structured additions, removals, category moves, and stable
  fingerprint changes as release evidence
- stable Core declarations require an intentional manifest update and review;
  removals or incompatible changes require a documented migration decision
- ABI remains explicitly unsupported until a later intent defines toolchain,
  standard-library, compiler-option, and symbol-version boundaries
- provisional and platform-internal categories must remain visibly distinct
  from the stable Core promise
- candidate library targets are static-only; shared-library packaging is not
  supported before version 1.0 and `BUILD_SHARED_LIBS=ON` is rejected
- Linux is the Tier 1 release-evidence platform; Windows remains Tier 2 until
  the M3 IOCP promotion gates complete, while still passing required functional
  and package-consumer CI
- macOS, BSD variants, other target systems, TLS, and experimental modules are
  rejected at configure time rather than represented by empty options or an
  implicit Linux backend
- the current all-rights-reserved `LICENSE` grants no external-use permission;
  an externally adoptable release remains blocked until the project owner
  deliberately publishes a license and matching package/SBOM metadata

## 5. Threading and Ownership Rules
- compatibility verification is a build-time repository guard and owns no
  runtime state
- metrics recorders may be called from producer loops but must not transfer
  producer ownership or block those loops on exporter I/O
- endurance and fault drivers own their client sockets and marshal every
  server mutation through the existing owner-loop APIs
- release automation owns evidence files only; it does not own runtime network
  objects or alter callback affinity
- the capacity server applies its finite send-buffer request inside each
  connection's established callback on that connection's owner loop
- this intent adds no re-entrant runtime callback before the metrics exporter
  contract is promoted and tested

## 6. Candidate Exit Gates
1. compatibility manifest and guard pass in every ordinary CI producer
2. provisional metrics exporter contracts pass under concurrency and teardown
3. fixed Release benchmarks stay within reviewed same-runner regression budgets
4. the same candidate commit produces strict paired Linux/Windows 10k mixed
   capacity evidence; 100k endpoint-attempt evidence, when claimed, comes from
   the dedicated capacity lane with the same immutable identity
5. fault-injection contracts pass on Linux and Windows
6. one frozen commit completes the 24-hour candidate endurance gate
7. the same frozen commit completes the 72-hour release endurance gate
8. candidate promotion revalidates the 10k pair plus 24-hour evidence; release
   promotion revalidates the dedicated 100k pair plus both 24/72-hour results,
   with exact source run/attempt identities and the same frozen commit
9. all supported platform, sanitizer, package, benchmark, and evidence gates
   are green and retained
10. any release offered for external adoption has an explicit owner-approved
   license; otherwise artifacts remain engineering previews with no use grant

## 7. Verification
- `tests/api/test_public_api_manifest.py` verifies the installed inventory,
  stability classes, package version, exported targets, and stable-header
  fingerprints; proves that missing, modified, misclassified, or tampered
  declarations are rejected; and exercises deterministic historical API diff
  and compatibility-decision semantics
- `tests/ci/test_performance_regression.py` verifies the fixed 12-scenario,
  three-repetition baseline/candidate matrix, reviewed budgets, same-runner
  workflow wiring, retained evidence, and a real failing-regression fixture
- `tests/ci/test_core_capacity_matrix.py` verifies the independent 12-scenario
  Core capacity baseline/candidate profile, matching Linux/Windows parameters,
  reviewed budgets, and the evidence-only Linux accept-topology decision
- `tests/cmake/test_capacity_profile_contract.py` verifies v1/v2 compatibility,
  the scale-ready v3 mixed profile, exact bounded reader/probe lifecycle
  accounting, the 10k candidate and 100k dedicated parameter sets, and paired
  evidence failure fixtures
- `tests/integration/resilience/test_fault_injection.cpp` verifies the declared
  reset, callback, overload, recovery, and forced-shutdown profiles
- `tests/ci/test_endurance_gate.py` verifies the uninterrupted-process 24/72-
  hour duration contract, heartbeat/checkpoint evidence, exact source-attempt
  wiring, and the candidate/release promotion manifest

## 8. Review Checklist
- Is every release claim backed by structured same-commit evidence?
- Did a stable declaration change without an explicit compatibility decision?
- Are platform and compiler support boundaries stated precisely?
- Can any metrics, fault, or endurance path block or mutate a non-owner loop?
- Are 24-hour and 72-hour results real durations rather than scaled substitutes?
