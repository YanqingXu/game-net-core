# M4 External Release Preflight — 2026-08-23

Status: **PREFLIGHT COMPLETE / OWNER AUTHORIZED / LICENSE TRANSITION ACTIVE / NO RELEASE**

Audit base: `4b63dac037595551652097f52504d4275a850c45`

Audit tree: `7451b08736590b05eb54c0f70d37751afd8f00c2`

This record captures the immutable engineering audit performed before the
licensing decision. The owner subsequently authorized Apache-2.0 and
conditional publication on 2026-08-23; the exact statement and its non-waived
release conditions are recorded in
[`m4_license_authorization_2026-08-23.md`](m4_license_authorization_2026-08-23.md).
The machine-readable inventory and authorization state are
[`m4_external_release_preflight_2026-08-23.json`](m4_external_release_preflight_2026-08-23.json).

This is an engineering provenance and release-readiness audit, not a legal
opinion. The project owner has supplied the required confirmation of licensing
authority; final release evidence remains an engineering gate.

## Audited external state before authorization

- `YanqingXu/game-net-core` is already a **public** GitHub repository with
  `main` as its default branch. Making the source repository public is therefore
  not a remaining M4 operation.
- GitHub classifies the current license as `Other`. The 130-byte Git blob at the
  audit base is all-rights-reserved and explicitly grants no license; its
  SHA-256 is
  `f80b9f62b48cc8d6d075cd7eb24da26881d95dbdd782239475261b0e267e37c8`.
- The only GitHub Release is the `v0.2.0-phase4-preview` prerelease published at
  `2026-07-11T21:38:05Z`. There is no `v0.3.0` tag or Release.
- The final internal candidate remains
  `v0.3.0-internal-candidate.1@0c3012449ae36fa32656da33c4d1161f5129cde7`.
  Its SBOM declares `LicenseRef-Proprietary-All-Rights-Reserved`, has four
  package records, and has no file-level inventory.

## Source, generated asset, and third-party inventory

The immutable audit tree contains 564 regular files, no symlinks, no
submodules, no Git LFS objects, no tracked `mini_trantor` subtree, and no
tracked archive or native binary. Repository history reports only the project
owner name, with two email addresses. One file contains a copyright marker
(`LICENSE`); no tracked file contains `SPDX-License-Identifier`.

No vendored third-party library, generated protocol source, external test
corpus, or benchmark dataset was identified by the tracked-tree and source
marker audit. CMake, Ninja, GCC/binutils, MSVC, the Windows SDK, pthread, and
Winsock are build or system prerequisites and are not bundled in the project
packages.

The six binary PacketFramer fuzz seeds are project-generated test assets. Their
byte content is fully defined by
`tests/fuzz/generate_packet_framer_corpus.py`; the manifest records the size and
SHA-256 of every output, and the governance test regenerates/checks their exact
Git-object content.

These negative findings narrow the review surface but do not establish legal
ownership or license compatibility. Owner confirmation remains mandatory
before relicensing.

## Post-M3 promotion impact

The internal candidate `0c30124` and this audit base have no diff under
`include/`, `api/public_api_manifest.json`, the root `CMakeLists.txt`, or
`cmake/`. The reviewed v0.3 public surface and package entry points therefore
have not drifted.

M3 did require Core runtime correction `736a090`. Relative to the internal
candidate, three private IOCP/TCP implementation files changed by 45 insertions
and 24 deletions:

- `src/core/net/TcpConnection.cc`
- `src/core/net/platform/IocpTcpTransport.h`
- `src/core/net/platform/IocpTcpTransport_win.cc`

That runtime change invalidates reuse of the internal candidate as an external
promotion commit even though its public surface is unchanged. After licensing
metadata and release automation are finalized, M4 must select one new exact
commit and rerun the complete M2 matrix.

## Release engineering gaps to close after authorization

At audit time, the repository workflows and validators covered CI, capacity,
benchmark, repeat/fault, and 1h/3h evidence, but final assembly was a one-time
ignored local operation. That gap is now closed by
`tools/assemble_release.py`, `tools/verify_release_bundle.py`, and the
byte-reproducibility/tamper contract in
`tests/cmake/test_release_assembler.py`. The assembler reads the immutable Git
object, accepts explicit install/evidence inputs, and emits source/binary/
evidence archives plus a file-level SPDX 2.3 inventory; the verifier accepts
the official SPDX 2.3 JSON schema.

The remaining release-engineering gap is clean external Linux/Windows current-
package consumers plus v0.2-to-v0.3 upgrade consumers. No release asset may be
represented as externally adoptable evidence until those consumers and the
full matrix share one exact promotion commit.

## Authorized execution sequence

Once the owner explicitly confirms licensing authority, authorizes the
Apache-2.0 transition, and authorizes publication after passing gates, execution
is fixed as follows:

1. Replace `LICENSE` with the canonical Apache License 2.0 text and synchronize
   source SPDX declarations, README, package metadata, `NOTICE`, third-party
   notices, SBOM fields, and known limitations.
2. Add a deterministic, tracked release assembler and contract tests for
   archive paths, contents, hashes, SPDX 2.3 schema, notices, and evidence
   identity.
3. Add clean Linux/Windows external package consumers and v0.2-to-v0.3 upgrade
   consumers.
4. Freeze one exact final commit and run Linux Debug/Release, ASan/UBSan, TSan,
   Windows Debug/Release/IOCP, the 129-test inventory, API/repository guards,
   paired benchmark, 10k/100k capacity, repeat-50, and fault injection.
5. Run an uninterrupted `candidate-1h`, then `release-3h`, against that same
   commit without an evidence waiver.
6. Rebuild packages twice, verify byte reproducibility, extracted consumers,
   SPDX SBOM, evidence index, `SHA256SUMS`, and known-limit records.
7. Only after every row passes, create the annotated `v0.3.0` tag, publish the
   GitHub Release and canonical assets, redownload them, and reverify hashes.

Any runtime, public API, package, license, or evidence-tool change after the
freeze selects a new promotion commit and restarts the applicable matrix. A
failed non-waivable gate produces `NO-PROMOTION`; it is not converted into a
release waiver.

## Authorization received

On 2026-08-23 the owner explicitly confirmed all three points:

1. the owner confirms authority to license every distributed project-owned
   file under Apache-2.0;
2. the owner authorizes the Apache-2.0 repository transition;
3. the owner authorizes creation and publication of `v0.3.0` only after the
   exact-commit gates above pass.

The repository license transition and deterministic release assembler are now
implemented. M4 remains open until external/upgrade consumers, the complete
promotion matrix, final packages, SBOM, tag, GitHub Release, and redownload
verification all pass.
