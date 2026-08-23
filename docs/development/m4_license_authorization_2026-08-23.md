# M4 Apache-2.0 and Publication Authorization — 2026-08-23

Status: **OWNER AUTHORIZED / LICENSE TRANSITION EXECUTED / RELEASE GATES PENDING**

The project owner supplied the following explicit statement:

> 我确认有权将 v0.3.0 拟分发范围内的项目文件以 Apache-2.0 许可；授权进行
> Apache-2.0 许可证切换；并授权在全部证据门通过后创建和推送 `v0.3.0` tag、发布
> GitHub Release 及其资产。

This authorization closes the legal-decision gate identified by the M4
preflight. It authorizes the repository-wide Apache-2.0 transition and permits
publication only after every non-waived exact-commit M4 gate succeeds.

The authorization does not waive CI, sanitizer, capacity, performance,
repeat/fault, 1h/3h endurance, package-consumer, reproducibility, SBOM, notice,
checksum, or release-asset verification. It does not authorize moving or
reusing a failed promotion tag. A failure after freeze produces
`NO-PROMOTION`, followed by a new exact candidate after remediation.

The license-transition tree must contain:

- the canonical Apache License 2.0 text in `LICENSE`;
- project attribution in `NOTICE`;
- the audited dependency boundary in `THIRD_PARTY_NOTICES.md`;
- canonical Apache-2.0 SPDX headers on project source files;
- `Apache-2.0` CMake package and public-manifest metadata;
- installation of all three legal/notice files;
- an SPDX 2.3 release SBOM declaring and concluding `Apache-2.0` for project
  packages and files.

The final SBOM and release packages bind the later immutable promotion commit;
they are deliberately not claimed by this authorization record alone.
