# API Compatibility Policy

`api/public_api_manifest.json` is the machine-readable compatibility boundary
for the 0.3 production-candidate line.
The frozen line installs as package version `0.3.0`; its CMake project version,
manifest package version, exact-version consumer, and baseline label must agree.

## Compatibility Classes

- `stable_core`: source compatibility is required within the 0.3 line. Any
  declaration-level drift must be reviewed and recorded by updating the
  manifest fingerprint. Removing or incompatibly changing a declaration also
  requires a migration decision in the release notes.
- `provisional`: the Phase 4 protocol, transport, session, logic, and broadcast
  APIs remain available for evaluation but may change before 1.0.
- `platform_internal`: backend and operating-system integration headers are
  installed today for implementation reasons but are unsupported application
  interfaces.

The project does not guarantee ABI compatibility before 1.0. Compiler, C++
standard-library, build-option, and runtime combinations are therefore not
interchangeable binary contracts. Consumers should rebuild against each 0.x
release.

## Change Procedure

Run:

```text
python tests/api/test_public_api_manifest.py
```

The guard rejects missing, newly unclassified, or multiply classified public
headers; target inventory drift; package-version drift; and any unrecorded
stable Core declaration change. Comments and whitespace are excluded from the
stable fingerprints.

A deliberate stable API change must update the public contract, direct tests,
manifest fingerprint, and release notes in one reviewed change. Updating a
fingerprint alone is not evidence that the change is compatible.
