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

The authorization also permits creation and publication of `v0.3.0` only after
all exact-commit gates pass. The license transition therefore does not claim
that `v0.3.0` has already been released; no release tag or asset may be
published before the promotion matrix and final artifact verification succeed.
