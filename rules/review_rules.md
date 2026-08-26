# review_rules.md

## 1. Review Order
For important modules, review in this order:
1. intent
2. public contract
3. invariants
4. thread-affinity correctness
5. ownership correctness
6. lifecycle correctness
7. failure semantics
8. implementation details
9. tests

## 2. Review Focus
Review should ask:
- Is the module solving the right problem?
- Are boundaries clear?
- Are invariants explicit?
- Are thread rules preserved?
- Is destruction safe?
- Are test contracts sufficient?

## 3. High-Risk Areas
The following areas always require focused review:
- cross-thread scheduling
- callback dispatch
- remove-before-destroy logic
- registration consistency
- state transitions
- coroutine suspend/resume integration points

## 4. Code-Smell Review Heuristics
Code smells are prompts for deeper review, not automatic rejection. A reviewer
should identify the affected invariant, boundary, failure mode, or test contract
before requesting a refactor.

Look for:
- a function mixing validation, state transition, ownership changes, I/O, and
  user callback dispatch at several abstraction levels
- deep nesting or a long conditional chain that hides the normal path, rollback,
  terminal state, or exact-once cleanup
- temporal coupling where correctness depends on undocumented call order
- query-like names that hide mutation, cross-thread scheduling, resource release,
  or user callback invocation
- clusters of boolean parameters or scattered boolean fields encoding an
  implicit state machine
- generic `Manager`, `Utils`, or `Helper` types accumulating unrelated ownership
  or lifecycle responsibilities
- duplicated invariant checks, error policy, ownership transfer, or callback
  ordering that can drift independently
- comments that restate syntax, contradict current behavior, or carry design
  authority that belongs in intent/rules/docs
- speculative abstractions justified only by possible future modules or deferred
  intents
- tests coupled only to implementation details while public failure, lifecycle,
  ordering, or threading behavior remains unproved

Explicit control flow and limited duplication may be preferable when extraction
would merge different owner threads, lifecycles, or public contracts. A smell
becomes a contract defect when it introduces hidden ownership, an undeclared
cross-thread mutation path, ambiguous callback re-entry, unsafe destruction,
silent critical failure, or behavior not covered by the required tests.

## 5. PR Standard
Each PR for a core module should contain:
- an intent reference whose metadata status is `active`
- the current roadmap phase/gate reference
- answers to the 5 core-module change gate questions
- public interface
- implementation
- tests
- diagram/doc updates if lifecycle-sensitive

A `deferred` or `legacy` intent cannot authorize implementation. Promotion must
first update the intent body against the current repository, change its metadata
and index catalog, and add the matching rule/test/evidence surface.

## 6. Core Module Change Gate
- Which loop/thread owns this module?
- Who owns it and who releases it?
- Which callbacks may re-enter?
- Which operations are allowed cross-thread, and how are they marshaled?
- Which specific test file verifies the change?

## 7. Review Checklist Example
- Does this change violate existing intent?
- Does it add hidden ownership?
- Does it create a non-owner-thread mutation path?
- Does it break callback ordering?
- Does it weaken remove-before-destroy discipline?
- Does it require updating docs/tests/diagram?

## 8. Forbidden
- review only code diff without intent context
- approve complex lifecycle changes without tests
- approve thread-affinity changes without explicit rule update
