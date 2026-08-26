# coding_rules.md

## 1. General
- Code must prioritize correctness, clarity, and maintainability
- Avoid over-design in v1
- Public API names should reflect reactor domain concepts clearly
- Implementation should align with corresponding intent file

## 2. Header Rules
- Each core header should begin with a short Chinese explanation block
- Header should declare responsibility clearly
- Avoid leaking unnecessary internal dependencies into headers
- Prefer forward declarations when possible

## 3. Class Design
- One class, one core responsibility
- Avoid “manager” classes without precise domain meaning
- Prefer explicit constructors
- Delete copy when ownership/thread semantics are unclear
- Use move only when semantics are safe and intentional

## 4. Error Handling
- Do not silently ignore critical errors
- Distinguish recoverable vs fatal errors
- Record enough context for debugging
- Do not convert every low-level error into exception throwing in reactor core

## 5. State Modeling
- Prefer enum-based explicit state machines over multiple loosely-coupled bool flags
- State transitions should be narrow and reviewable
- Lifecycle-sensitive state changes should be centralized

## 6. Callback Rules
- Callback execution path should be clear
- Avoid deeply nested callback dispatch logic
- Document callback ordering for lifecycle-sensitive modules
- Avoid invoking user callbacks from ambiguous thread contexts

## 7. Thread Safety
- If a method is loop-thread only, name/document/test it as such
- Cross-thread interaction must go through defined mechanisms
- Do not casually add mutexes to compensate for unclear ownership design

## 8. Comments
- Comments should explain why, not restate obvious code
- Key lifecycle or threading logic should have local comments
- Large design reasoning belongs in intent/rules/docs, not random inline comment blocks

## 9. Readability and Local Design
- Keep a function at one main level of abstraction; separate protocol validation,
  state transition, resource mutation, and callback dispatch when combining them
  hides the lifecycle
- Prefer early exits when they make the normal path clearer, but not when they
  obscure exact-once cleanup, rollback, or callback ordering
- Give non-trivial conditions domain names when the name exposes an invariant or
  transition better than the raw expression
- Names must make consequential side effects discoverable, especially user
  callback invocation, cross-thread scheduling, ownership transfer, and resource
  release
- Avoid clusters of boolean mode parameters in public APIs; use an enum or a
  focused options type when the values select distinct behavior
- Keep helpers, aliases, and dependencies in the narrowest responsible module;
  do not grow generic `Manager`, `Utils`, or `Helper` containers for unrelated
  reactor concerns
- Remove duplication when it repeats the same invariant, ownership rule, or
  failure policy; do not couple coincidentally similar code whose owner threads,
  lifecycles, or contracts differ
- Function length alone is not a violation; cohesion, visible state transitions,
  and the number of abstraction levels determine whether extraction is needed

## 10. Testing
- Every public behavior contract should be testable
- Every lifecycle-sensitive module must have contract tests
- Every cross-thread API must have threading-related tests

## 11. AI-Specific Requirement
- Generated code must reference the intent and rules it implements
- Generated code should remain small enough for human audit
- Avoid generating large opaque helper abstractions without explicit intent support
