# io_uring backup restoration review

## Scope and preserved identity

The restoration preserves these seven original commits (oldest first):

1. `7a82ed3532367153ac17d6f20b53bf3694902987` — installed native Linux acceptance.
2. `49f7384839f4c1b2d8c9cd2ebb9d593bee410331` — RSS high-water evidence.
3. `e9308813baca09f2d8d4fe017bf0be77a77686de` — kernel/WSL identification.
4. `8410c6a867ad48f2b5d1f62810de05aaf630bb8a` — steady-state CPU attribution.
5. `6b8a3d00446297c701c2e57a5ab7139e79a590e3` — isolated callback Send coalescing.
6. `afe540959f8e0d2a75842af5d935f381d0571f5b` — Core test-hook comparison.
7. `7dbc79a7fcaeae65d2c673b430a7b5bfda9b1248` — dispatch-budget sweep.

The verified Git bundle SHA-256 is
`eb907ced6a018c6ab4f0870a3023d9e08676c168cd6356482a7a8da9c3f84384`.
The integration retains main's `fac8ce2bc3ad5995eeff25d90cd97d30edaced27`
documentation changes. The final diff does not modify production `src/`, public
headers, existing tests, package/API manifests, intents, or ownership rules.

## Static review and corrections

Review covered all eleven restored files and the existing Pump implementation:
owner-thread callbacks, bounded frame/burst sizes, peer identity, Send admission,
EOF/half-close, native retirement, timer cancellation, build/manifest provenance,
and workflow execution boundaries. No confirmed new production security path was
found. The Send-flush delta remains an experimental patch applied only to a
separate CI checkout, not a runtime change in main.

Confirmed engineering findings corrected during integration:

- Medium, confirmed: branch-only triggers and a hard-coded short-retention
  artifact made the experiments non-repeatable after branch cleanup. Workflows
  now support manual execution and scoped PR/main checks. The dispatch sweep
  consumes a freshly replayed same-run variant instead of downloading old bytes.
- Medium, confirmed: replay validated SHA syntax, but could not bind it to an
  externally obtained expected source/consumer identity. All replay CLIs now
  accept those expectations; CI passes the immutable workflow SHA for both.
- Low, confirmed: the variant contract gate checked eleven broadly named tests,
  not the exact eleven required contracts. It now rejects missing, substituted,
  duplicate, failed, or skipped contract results.

Fresh dispatch evidence uses schema v2 and binds its predecessor manifest,
both installed archive hashes, source delta, patch, and exact consumer binary.
Historical schema v1 and its original predecessor pin remain unchanged. External
auditors must replay both fresh artifacts and independently compare the CPU
manifest SHA-256 with the dispatch predecessor digest; a manifest is not a
cryptographic authority by itself.

## Validation and merge boundary

`test_evidence.py` covers 18 portable checks for native kernel identification,
revision identity, file hashes/inventory, duplicate admission ledgers, exact
contracts, trial plans, timer cadence/cancellation, syscall windows, predecessor
identity and archive/consumer binding. They are not native runtime evidence.

Merge requires the restoration PR's authoritative normal CI and native acceptance
and paired CPU/dispatch workflows to pass, followed by downloaded artifact replay
and identity/hash comparison. Run IDs, checked SHA, and artifact verification
results belong in the PR audit comment. Failures must be investigated, not waived.
Windows checks cover portability; only native Linux provides io_uring evidence.

No performance promotion is inferred: HP0/HP8 remain DEFER, the component remains
experimental/default-OFF, and fixed-duration production gates are unchanged.
No release/tag or scheduled automation is created by this restoration.
