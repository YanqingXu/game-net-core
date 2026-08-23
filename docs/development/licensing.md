# Licensing Status

The project owner confirmed licensing authority and authorized the repository
transition to the Apache License, Version 2.0 on 2026-08-23. The current
top-level `LICENSE` contains the canonical Apache-2.0 text. `NOTICE` records the
project attribution, and `THIRD_PARTY_NOTICES.md` records the audited dependency
and generated-asset boundary.

The Apache-2.0 scope covers project-owned source, headers, tests, build and CI
configuration, tools, documentation, and deterministic test assets unless a
file explicitly states another license. C/C++, Python, CMake, shell/PowerShell,
and workflow source files carry canonical copyright and
`SPDX-License-Identifier: Apache-2.0` headers. Formats that do not admit comments
are covered by the repository license and release SBOM instead of receiving an
invalid inline field.

Installed CMake packages export `GameNetCore_LICENSE=Apache-2.0` and paths to
the installed `LICENSE`, `NOTICE`, and `THIRD_PARTY_NOTICES.md`. Every source
and binary release archive must include those files, and the SPDX 2.3 release
SBOM must declare and conclude `Apache-2.0` for project packages and files.

The authorization permitted creation and publication of `v0.3.0` only after
all exact-commit gates passed. Those gates and final artifact verification
succeeded for promotion commit
`8e4a6edfe22ca43e3308e36ec31bf7f2dea14ac7`; the annotated tag and stable
Apache-2.0 GitHub Release were published on 2026-08-23. Exact evidence and
asset hashes are recorded in `docs/development/releases/v0.3.0.md`.
