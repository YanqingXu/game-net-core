# Intent-Driven Development

game-net-core keeps the original repository's intent-driven workflow.

The development order is:

```text
intent
  -> invariants
  -> threading rules
  -> ownership rules
  -> public contracts
  -> tests
  -> implementation
```

## Why This Exists

The core networking code is lifecycle-sensitive and thread-affine. Starting
from implementation details makes it too easy to add hidden ownership transfer,
ambiguous callback execution, or non-owner-thread mutation paths.

Intent files keep the design explicit. Tests enforce the intent as executable
contracts.

## Working Rules

- Important module changes must start from the relevant intent file.
- Public APIs need contract tests.
- Cross-thread behavior needs threading tests.
- Lifecycle-sensitive modules need ownership and teardown tests.
- Deferred intent files are references, not automatic scope approval.

## Change Gate

Every core-module change must answer:

- Which loop/thread owns this module?
- Who owns it and who releases it?
- Which callbacks may re-enter?
- Which operations are allowed cross-thread, and how are they marshaled?
- Which specific test file verifies this change?
