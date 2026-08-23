# M11 v1.0 Stabilization / Release Readiness Audit

Date: 2026-08-24

Core baseline: `a2977c90374aa6c08a54d2d569ff369c51717345`

## Decision

- v1.0 release: `DEFER` / `NO-RELEASE`;
- no version bump, ABI promise, compatibility-line change, stable-category
  promotion, tag, GitHub Release, source/binary asset, or empty v1.0 package is
  created;
- stable `v0.3.0@8e4a6ed` remains the external release baseline;
- the M1–M11 plan is fully adjudicated. There is no open implementation or
  governance front after this audit; v1 work may resume only through a new
  active v1 intent and exact evidence plan that closes the listed gates.

The current v0.3 code and release tooling are healthy, but the repository does
not define or prove a v1 contract. Calling the existing package `1.0.0` would
replace missing API/ABI, migration, platform, endurance, and artifact evidence
with a version number.

## Current Positive Evidence

The audit first rebuilt the current Windows Release graph instead of relying on
stale binaries:

- the pre-reconfigure build registered 129 tests and failed one old
  `dedicated_fixed_tick_profile` binary at `timerCancelAccepted == 1`, for an
  honest 128/129 result;
- after current-source reconfigure/rebuild, the graph registered 130 tests;
- the fixed-tick contract passed 20/20 consecutive current-binary runs;
- the complete current Windows Release graph passed 130/130: 8 unit, 108
  contract, and 14 integration, including 103 threading and 108 lifecycle
  labels;
- a fresh current Windows install tree and both stable/provisional consumers
  passed 2/2;
- the current comparison against the reviewed v0.3 surface reports
  `has_changes=false`, no compatibility decision, and no stable-surface review;
- ten API/experimental-manifest/install/release-assembler/release-consumer/
  performance/capacity/endurance-tool/governance contracts passed 10/10.

The old 129-test failure was not reproducible after the required rebuild, so no
runtime fix is inferred or made. The tooling tests prove that v0.3 gates reject
bad evidence; they do not produce v1 release evidence.

## Non-Waivable v1 Gaps

### 1. Version and compatibility contract

Every active release artifact still names v0.3:

- CMake project version is `0.3.0` with `SameMinorVersion` compatibility;
- `api/public_api_manifest.json` declares package `0.3.0`, compatibility line
  `0.3`, and `v0.3.0-production-candidate`;
- `candidate_freeze.json`, install consumers, release metadata, assembler,
  verifier, and upgrade gate are v0.3-specific;
- `production_candidate_release.intent.md` explicitly freezes package `0.3.0`
  and says there is no ABI compatibility guarantee before 1.0;
- `api_compatibility.md` defines only within-0.3 source compatibility and keeps
  protocol/transport/session/logic/broadcast plus metrics provisional;
- shared libraries are still rejected and there is no reviewed compiler,
  standard-library, option, symbol, allocator, exception, or runtime boundary
  for a v1 ABI promise.

There is no active v1 release intent, v1 public manifest/baseline, independent
v1 API/ABI review, or decision about which provisional targets become stable.

### 2. Migration consumer

The executable upgrade fixture proves v0.2-to-v0.3 stable-Core consumption.
There is no `0.3 -> 1.0` source migration consumer, migration guide, accepted
break list, or exact 1.0 package for that consumer to resolve.

### 3. Same-commit platform and quality matrix

The baseline has a local Windows Release 130/130 result only. It has no
same-commit M11 evidence for:

- Linux Debug/Release and Windows Debug;
- Linux ASan/UBSan and TSan;
- applicable fuzz targets;
- paired performance and Linux/Windows capacity numbers;
- dedicated 100k capacity;
- full fault/repeat matrix beyond the ordinary local Release test;
- external gateway one-hour and Core three-hour endurance.

Historical v0.3, M3 gateway, IOE-X14, and M6 evidence remains valid for its own
immutable commits but cannot be relabeled as `a2977c9` v1 evidence. No waiver is
requested or implied.

### 4. Release artifacts and publication

No v1 release metadata, promotion manifest, Linux/Windows install trees,
source/binary archives, checksums, SPDX SBOM, evidence archive, third-party
notice verification, fresh-download verification, or 0.3-to-1.0 extracted
consumer exists. Local and remote tag inspection found no `v1*` tag. No tag,
push, hosted workflow, or GitHub Release action is authorized by passing only
the local v0.3 contracts.

## M11 Gate Matrix

| Gate | Current result | Decision |
| --- | --- | --- |
| independent stable API/ABI policy review | Missing; current policy is v0.3 source-only and pre-1.0 ABI unsupported | Blocking |
| 0.3-to-1.0 source migration consumer | Missing | Blocking |
| Linux/Windows Debug/Release | Current Windows Release 130/130 only | Blocking |
| sanitizer/TSan/fuzz | No same-commit M11 execution | Blocking |
| benchmark/capacity/fault | Tool contracts pass, but no same-commit paired/dedicated evidence | Blocking |
| gateway 1h / Core 3h | No same-commit execution or waiver | Blocking |
| package/SBOM/checksum/license set | v0.3 tooling exists; no v1 inputs or artifacts | Blocking |
| single promotion commit | No v1 candidate/promotion commit or manifest | Blocking |
| lifecycle residue | Current Windows Release is green after rebuild; other required lanes absent | Incomplete |

## Resume Requirements

A future v1 effort must begin with an active v1 release intent and contract
tests, not a version edit. At minimum it must:

1. decide stable versus provisional/experimental targets and state the exact
   source/ABI/toolchain/build-option support promise;
2. create independently reviewed v1 manifests/baselines and a 0.3-to-1.0
   migration consumer/guide;
3. generalize v0.3-specific version, assembler, verifier, release metadata, and
   consumer tooling without weakening deterministic/tamper checks;
4. select one immutable promotion commit and run every required platform,
   sanitizer/race/fuzz, performance/capacity/fault, package, 1h/3h, SBOM,
   license, and fresh-download gate against it;
5. publish only after all non-waived gates are green and any permitted waiver
   is explicit, owner-authorized, same-commit, and never described as a pass.

## Ownership, Re-entry, and Cross-Thread Decision

This audit and `NO-RELEASE` decision create no runtime object, callback, owner,
or cross-thread path. Existing EventLoop, TcpConnection, SessionManager,
gateway, and experimental io_uring ownership contracts remain unchanged. The
stale-build observation required only a current rebuild and introduced no code
change.

## Repository Verification

`tests/cmake/test_migration_status_contract.py` binds this audit, exact
baseline, 128/129 stale-build and 20/20 plus 130/130 current-build truth,
Windows install 2/2, tooling 10/10, v0.3 zero-diff, every blocking v1 gate,
`DEFER` / `NO-RELEASE`, stable v0.3 retention, absence of local `v1*` tags, and
the final no-open-front plan state.
