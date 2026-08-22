# M3 real-gateway closure — 2026-08-23

M3 is closed by the independent private `gamenet-game-gateway` repository at
closure commit `0a8fe1e43cb11ac32daa8f9266d3b84924736e67`.

The gateway consumes only installed `GameNetCore 0.3.0` targets. Q1 found one
Core correctness blocker in Windows IOCP revoked-completion retirement; Core
commit `736a0907e90dbd0373cd652283618794c31020df` fixed it without public API
manifest drift. Later Queued Event and Sharded Hybrid slices required no Core
source-private helper or additional public capability.

Closed gateway evidence includes:

- true TCP Auth/Session, duplicate generation, disconnect/reconnect, and kick;
- bounded logic/Lua execution, exception and blocking isolation, typed
  saturation, recovery, and callback re-entry;
- generation-safe output, cross-owner broadcast, persistence, slow-client
  pressure, and deterministic aggregate shutdown;
- independent Sharded Hybrid key placement, per-cell FIFO/fixed-head ordering,
  saturation isolation, recovery, and sharded broadcast;
- deterministic traffic replay and eight fault/recovery profiles on Windows
  MSVC/IOCP and Linux GCC/epoll Release, plus Linux ASan/UBSan;
- one uninterrupted Linux/epoll Release process on exact gateway commit
  `4e2457e81f0ba2154b6aca2e6cd945daf793fbba` and Core `736a090`: 3,743 complete
  cycles, 3,600.015 child-monotonic seconds, 3,600.023 supervisor-monotonic
  seconds, and 32 KiB peak RSS growth.

The gateway feedback ledger classifies every finding. No unresolved Core
correctness blocker or missing broadly reusable capability remains. M4 is now
eligible to start, but Apache-2.0 relicensing and public release require explicit
project-owner authorization. Because `736a090` changes the runtime after the M2
candidate commit, M4 must select a new final promotion commit and rerun the full
same-commit promotion matrix after all release-governance changes land.
