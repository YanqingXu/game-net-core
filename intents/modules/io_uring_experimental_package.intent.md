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

- `tests/cmake/test_experimental_io_uring_install_contract.py`
- `tests/api/test_experimental_io_uring_api_manifest.py`

## 6. HP8 Review Boundary

HP8 records `DEFER` for additional backend work. No comparable post-HP1-HP6
production data path or clean HP0 Linux/Windows ledger exists, so multishot
accept/recv, provided buffers, registered files, send bundles and SQPOLL remain
separate unstarted experiments. Existing IOE-X1-X15 packaging and explicit
Linux opt-in behavior are unchanged; there is no default selector or promotion.
