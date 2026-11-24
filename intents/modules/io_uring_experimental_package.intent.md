---
status: active
target: GameNet::experimental_io_uring
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# Module Intent: io_uring Experimental Package Surface

## 1. Intent

IOE-X15 turns the already proven Linux io_uring Server/Client composition into
an explicitly requested experimental package component. It changes artifact
reachability, not the production backend model: Linux production remains
epoll, Windows production remains IOCP, and no stable backend selector or
automatic io_uring choice is introduced.

## 2. Public Experimental Contract

- `GAMENET_ENABLE_EXPERIMENTAL` remains `OFF` by default and remains rejected
  on non-Linux systems.
- A Linux build with the option enabled installs the static target as
  `GameNet::experimental_io_uring` and installs only the header closure needed
  by `IoUringTcpServer` and `IoUringTcpClient` under
  `include/gamenet/experimental/io_uring/`.
- A consumer requests the component explicitly with
  `find_package(GameNetCore REQUIRED COMPONENTS experimental_io_uring)`.
  Requesting it from a default package fails at configure time; it never falls
  back to `GameNet::core` or silently selects epoll.
- The component is experimental and pre-1.0. It has an independent manifest,
  version note, header fingerprints, and compatibility policy. Its target and
  headers do not enter `api/public_api_manifest.json` or the stable v0.3
  compatibility decision.
- The installed surface exports no source-private proof-only
  `IoUringTcpConnectionDriver` or multi-owner composition façade. Those remain
  build-tree implementation/test assets.

## 3. Threading and Lifecycle

- Installation creates no new execution domain. Each Server or Client and its
  Engine, Pump, Hub, Adapter, connection state, lifecycle transitions,
  callbacks, observations, stop publication, and destruction remain on the
  caller-supplied EventLoop owner.
- Connection callbacks may re-enter the same owner-safe lifecycle methods
  already proven by IOE-X11–X14. Re-entry must revalidate phase and exact
  identity/generation before continuing.
- Server/Client configuration, lifecycle, observation, and destruction remain
  owner-only. Only an established connection Adapter retains the existing
  bounded foreign `trySend`, `tryShutdown`, and `tryForceClose` admission.
- The consumer owns and outlives the EventLoop and façade. The façade owns its
  source-private composition and releases it only after the existing physical
  final-drain summary is ready.

## 4. Failure and Compatibility Boundary

- Missing component, non-Linux enablement, absent target/header, invalid
  option, package-version mismatch, and consumer build/run failure are explicit
  gate failures.
- Default Linux and all Windows install trees must contain no experimental
  target, library, header, or component-success marker.
- Experimental installation does not authorize multishot operations,
  provided-buffer rings, registered/fixed files, zero-copy Send, SQPOLL, TLS,
  framing, DNS, game/business state, production backend replacement, or a
  stable public selector.
- No performance, capacity, or promotion claim is implied by packaging the
  previously verified implementation.

## 5. Verification

### Owner-local hot-path bookkeeping

The installed experimental implementation may use a fixed intrusive free-slot
chain instead of scanning the operation table. Only taking the exact terminal
notice returns its slot to that chain; CQE decoding, cancel-request completion,
and failed admission never free a notice-pending or active generation. Payload
and lease ownership still move with the terminal notice through consumer return.

The Hub may skip its close-retirement scan when no route is Closing. The owner
increments that count only on the first Closing transition and decrements before
publishing a retired route's callbacks. Reentrant close, generation reuse and
aggregate quit retain the existing scan order and physical-drain boundary.

Direct contracts: `test_io_uring_completion_engine.cpp` (full-table exhaustion,
terminal-notice retention and repeated generation reuse) and
`test_io_uring_tcp_connection_hub.cpp` (reentrant close/reuse and zero residue).
Same-host before/after measurements retain the unchanged epoll reference,
workload, affinity, budgets, installed consumer and raw replay. These changes
remain experimental; no default backend or HP0/HP8 promotion is implied.

### Owner-callback Send commit and payload initialization

The Pump may stage at most one successful Send during an already-running owner
completion consumer, and only with an SQ capacity greater than one. The next
successful operation flushes it together with that operation, or consumer exit
(including exception exit) flushes it before returning to dispatch. Cancellation
first flushes a staged Send so that the existing physically-submitted identity
contract is unchanged. Outside a consumer, and on a one-entry SQ, Send commits
immediately. No timer, cross-turn batch, extra queue, worker, or larger dispatch
budget is introduced. Quiesce still owns every admitted target until its terminal
CQE and notice consumer retire; cancel completion is not target retirement.

An Engine Send may initialize its owned byte vector directly from the caller's
range, avoiding a zero-fill followed by overwrite. It still takes an independent
snapshot before returning Accepted; caller mutation, embedded NULs, partial
completion, generation reuse and retained notices cannot alias that snapshot.
Recv initialization, finite capacity accounting, Adapter mailbox credits, Hub
FIFO ownership and the installed API remain unchanged. This is not zero-copy
and removes no allocation. Removing mailbox/Hub copies requires a separate
ownership design and is not authorized by this optimization.

Direct contracts: `test_io_uring_event_loop_pump.cpp` checks callback-only commit,
no-rearm and exceptional exit, multiple Send admission, cancellation, quiesce,
one-entry SQ and immediate non-callback progress; `test_io_uring_completion_engine.cpp`
checks byte-exact owned snapshots and lease/notice lifetime. A separate native
factorial experiment compares unchanged baseline, batch-only, copy-only and
combined packages, with fixed epoll control, dispatch budget 16, and at least 20
timer repetitions per selected arm. Historical experiment verifiers stay fixed.

- `tests/cmake/test_experimental_io_uring_install_contract.py`
- `tests/api/test_experimental_io_uring_api_manifest.py`

## 6. HP8 Review Boundary

HP8 records `DEFER` for additional backend work. No comparable post-HP1-HP6
production data path or clean HP0 Linux/Windows ledger exists, so multishot
accept/recv, provided buffers, registered files, send bundles and SQPOLL remain
separate unstarted experiments. Existing IOE-X1-X15 packaging and explicit
Linux opt-in behavior are unchanged; there is no default selector or promotion.
