# Commit-Bound Evidence Ledger Template

Use one copied section per integrated architecture slice. Evidence is attached
to an immutable commit; a dirty-worktree result may be recorded as local
preflight but cannot be promoted to commit or remote evidence.

## Slice Record Template

| Field | Required value |
| --- | --- |
| Slice | Plan identifier and one-sentence scope |
| Commit | Full 40-character commit SHA; never `HEAD` |
| Parent/baseline | Exact comparison SHA and baseline document |
| Intent/ADR | Active intent paths and accepted architecture decision |
| Owner/lifetime | Owner thread, owner/releaser, re-entry, cross-thread path |
| Public surface | `none`, or exact reviewed API diff artifact |
| Changed production paths | Explicit path list |
| Focused contracts | Exact command, test names, result, duration |
| Windows full gate | Exact command/result and artifact or local log identity |
| Linux full gate | Exact command/result and artifact or local log identity |
| Benchmark/capacity | Scenario, paired sample count, result, decision |
| Sanitizer/race evidence | Required lane and result, or reason not applicable |
| Review | Reviewer identity, scope, findings, disposition |
| Remote evidence | Run/attempt/artifact/hash, or explicitly `not collected` |
| Decision | `integrate`, `fix`, `revert`, or `promotion-only follow-up` |

## Integrity Rules

1. Record the commit only after the tree is committed and clean.
2. Do not copy evidence from an ancestor unless the changed path audit proves it
   remains applicable and the record labels that proof.
3. Local and remote results remain separate. Workflow presence is not evidence.
4. A benchmark beyond its investigation threshold requires paired reruns and a
   written accept/fix decision.
5. Failed or superseded evidence remains visible with its disposition.
6. Release/endurance evidence is promotion-only unless a slice changes the
   corresponding validator or lifecycle contract.

ARCH-G1 establishes this template. IOE-R1 is the first slice required to append
a completed, exact-commit record.
