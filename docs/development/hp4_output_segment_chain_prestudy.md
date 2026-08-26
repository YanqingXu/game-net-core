# HP4 Output Segment Chain Prestudy

Date: 2026-08-25

Decision: `DEFER`

Evidence class: development-only, no-syscall, non-promotion

## Scope And Lifecycle

The opt-in prototype owns a fixed-capacity ring of moved strings or shared
immutable string slices. The constructing connection/EventLoop owner alone may
enqueue, prepare, complete, fail, stop, cancel and destroy it. Foreign-thread
mutation is rejected; a formal cross-thread caller would first own its value
and use the existing bounded owner admission.

One batch borrows stable segment suffixes until completion and contains at most
16 views / 64 KiB. Partial completion advances exact offsets, one batch may be
in flight, and prepare/completion invoke no callbacks. Stop seals admission;
shutdown requires every accepted byte to be either completed or discarded once.

## Correctness Evidence

The focused Windows MSVC Release contract passed 1/1 and its MSVC Debug
AddressSanitizer run passed 1/1. It covers capacity, owned/shared lifetime,
stable suffix pointers, no concatenation, batch limits, FIFO, partial
cross-segment completion, in-flight rejection, invalid completion, stop/fail/
cancel settlement, foreign owner and shared broadcast generation.

Five unchanged production regressions passed 5/5: IOCP segmented write, IOCP
partial write, TCP output-memory budget, broadcast contract and multi-loop TCP
broadcast integration.

## Development Measurement

Ten sequential Release samples per payload used 256 endpoints and 100
iterations after an unrecorded warmup. The baseline allocated and concatenated
one header/payload string per endpoint; the candidate retained two shared
segments. All samples matched checksums/batch counts and reached zero residue.

| Payload | Concatenation median | Segment median | Median paired improvement |
| --- | ---: | ---: | ---: |
| 1,024 bytes | 951,000 ns | 1,570,350 ns | -65.10% |
| 16,384 bytes | 26,707,150 ns | 1,568,100 ns | 94.07% |

Each case modeled 25,600 baseline allocation events. Baseline copied
26,316,800 bytes at 1 KiB and 419,532,800 bytes at 16 KiB; the candidate
recorded zero allocation events and zero concatenation-copy bytes. The 16 KiB
baseline range was wide (3,808,400-29,414,100 ns), further limiting inference.

## Missing Evidence And Decision

The prototype invokes neither `writev` nor `WSASend`. It therefore does not
prove syscall reduction, platform vector limits, IOCP overlapped-storage
lifetime, readiness drain behavior, cancellation, socket-error mapping,
write-interest transitions or end-to-end P99/P999 latency. Clean fixed-lab HP0
pairing and native Linux evidence are also absent.

Because small payloads regress and native I/O evidence is missing, the decision
is `DEFER`. Production TcpConnection, TransportEndpoint, BroadcastDispatcher,
budgets, installed APIs and defaults remain unchanged; formal HP4 stays
`planned`, and the prestudy slot is released.
