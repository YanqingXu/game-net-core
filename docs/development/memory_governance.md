# Memory Governance

The M3 capacity model separates logical pending bytes from retained allocator
capacity and transport-internal fixed storage. Adding those values together is
valid only when each category has one owner and no storage is counted twice.

## Active accounting surfaces

| Storage | Accounting surface | Scope | Release point |
|---|---|---|---|
| admitted TCP output | `TcpOutputMemoryBudgetSnapshot` | connection / loop / server / optional global | write completion or terminal discard |
| `Buffer` capacity | `BufferRetentionSnapshot` | one owner-thread Buffer | hysteretic trim or Buffer destruction |
| `PacketFramer` ring capacity | `PacketFramerRetentionSnapshot` | one caller-owned framer | hysteretic trim/reset or destruction |
| Broadcast logical work | `BroadcastOutstandingSnapshot` | dispatcher / owner | task terminal result |
| AcceptEx slot vector | `NetworkFixedStorageRetentionSnapshot` | process | final shared pool-state destruction |
| IOCP completion workspace | `NetworkFixedStorageRetentionSnapshot` | process | Poller destruction |
| IOCP connection read chunk | `NetworkFixedStorageRetentionSnapshot` | process | final read completion followed by connection close |

`NetworkFixedStorageRetentionSnapshot` reports transport-internal pool, slab,
and fixed working storage that is not represented by the logical pending-byte
budgets. It intentionally does not count ordinary object-inline bytes,
general-purpose STL container overhead, operating-system socket/kernel memory,
thread stacks, or allocator metadata. Those values need platform RSS profiles
rather than invented byte precision.

The active Reactor/TCP implementation has no shared read pool and no shared
read slab. Both public fields are therefore exactly zero. Windows uses one
optional connection-local read allocation capped at 4 KiB; it never returns
that allocation across connections or EventLoops.

## Snapshot semantics

The fixed-storage snapshot is process-wide and cross-thread-safe. Producers
update relaxed atomic counters only at allocation/construction and
release/destruction boundaries, never per packet or per byte transferred.
Individual fields are low-frequency observations and may describe slightly
different instants while other threads are changing categories. Each current
counter remains exact and nonnegative, each category peak is monotonic, and
the separately tracked total peak records a real observed aggregate.

AcceptEx accounting follows the shared slot-vector lifetime. Stopping or
destroying an Acceptor can leave the current pool bytes nonzero while the IOCP
Poller still owns completion leases; the count reaches zero only after terminal
packets release those leases.

## Combined capacity evidence

[`capacity_profile.md`](capacity_profile.md) describes the real-TCP
slow-reader + Broadcast + recovery profile. It keeps logical pending,
dispatcher reservations, owner-loop Buffer retention, process fixed storage,
and observational RSS as separate categories, then validates their individual
bounds and terminal release points without double-counting connection-local
read storage.
