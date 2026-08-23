# v0.3.0 Release Packaging

`tools/assemble_release.py` is the only supported v0.3.0 external-release
assembler. It reads the source payload from an immutable Git commit rather
than from the working tree, packages explicit Linux and Windows install trees,
and includes one or more labeled evidence directories. It refuses to overwrite
an existing output directory.

The operation emits deterministic source tar/zip archives, Linux and Windows
binary archives, an evidence archive, canonical license/notice files, an SPDX
2.3 JSON SBOM with file-level SHA-1/SHA-256 checksums, a commit-bound evidence
index, a package manifest, and `SHA256SUMS`. Archive paths, entry order,
permissions, gzip metadata, and ZIP timestamps are normalized. Repeating the
operation with the same immutable source object and byte-identical install and
evidence inputs produces byte-identical output.

Example (PowerShell):

```powershell
python tools/assemble_release.py `
  --commit $candidateSha `
  --expected-commit $candidateSha `
  --linux-install-tree out/release-input/linux-install `
  --windows-install-tree out/release-input/windows-install `
  --evidence promotion=out/release-input/promotion-evidence `
  --evidence ci=out/release-input/ci-evidence `
  --promotion-manifest out/release-input/promotion-evidence/release-promotion.json `
  --output out/game-net-core-v0.3.0

python tools/verify_release_bundle.py `
  --bundle out/game-net-core-v0.3.0 `
  --expected-commit $candidateSha `
  --expected-version 0.3.0 `
  --spdx-schema out/tooling/spdx-2.3-schema.json
```

The final verifier must be run with the official SPDX 2.3 JSON schema. A valid
bundle is still not publishable until all promotion evidence in its evidence
archive passes the same-commit release gates.

The assembler itself requires the named promotion manifest to be inside a
labeled evidence root and rejects it unless it is a successful release-stage
v2 record for the exact source commit, dedicated-100k capacity, and completed
candidate-1h plus release-3h endurance evidence.
