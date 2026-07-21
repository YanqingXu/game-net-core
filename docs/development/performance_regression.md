# Performance Regression Gate

Phase 6 adds a performance gate without treating scores from different hosts
as comparable capacity claims. Each Linux/epoll and Windows/IOCP producer uses
one GitHub runner to build both the reviewed baseline commit
`2b1be4343f7c478eb40542451f30aad8ca474003` and the candidate commit.

## Matrix

`tools/run_performance_matrix.py` runs 12 fixed Release scenarios three times
per revision and keeps every raw JSON sample:

- Core echo at 1, 2, and 4 workers;
- Core idle-connection holds at 256 and 1,024 connections;
- Core slow-reader pressure at 4 and 16 clients;
- Phase 4 framing;
- Phase 4 logic queue at 20,000/4-producer and 40,000/8-producer scales;
- Phase 4 broadcast at 1,024 and 4,096 fanout.

The matrix manifest binds the commit, platform, backend, build type, executable
SHA-256, exact parameters, sample paths, and sample hashes. Three repetitions
allow the comparator to use medians rather than a single noisy observation.

## Budgets

`benchmarks/performance_regression_budgets.json` is the reviewed budget
contract. Throughput and operation-rate metrics are higher-is-better; P99 and
working-set deltas are lower-is-better. Each metric declares a maximum relative
regression and an optional absolute noise allowance. The comparator fails
closed on missing scenarios, parameter drift, non-finite values, sample/hash
drift, an unreviewed baseline SHA, or a failed median budget.

The allowances are regression tripwires, not production capacity targets.
Changing a budget or baseline requires a reviewed change to the JSON contract,
workflow guard, and release evidence. A candidate must not update the baseline
to itself merely to make the gate pass.

## Evidence

Each platform artifact retains 72 raw sample documents (12 scenarios × 3
repetitions × 2 revisions), two matrix manifests, the semantic benchmark
manifest, the job manifest, and `gamenet.performance_regression.v1`. The paired
evidence job rehashes the samples, requires both platform regression results to
pass against the same baseline and budget contract, and still avoids comparing
Linux values directly with Windows values.

A local Windows/IOCP preflight of the gate passed all 12 comparisons with 36
candidate and 36 baseline samples. This is tool validation only: publication
still requires the workflow-retained Linux/epoll and Windows/IOCP artifacts to
name the same immutable candidate commit.
