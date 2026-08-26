# Performance Deployment Guide

This is a measurement checklist, not a universal tuning recipe. Record the
before/after configuration with the exact executable, workload and host. Change
one dimension at a time and retain throughput, P99/P999, CPU, RSS, recovery and
shutdown guardrails.

## CPU And NUMA Placement

- inventory physical cores, SMT siblings, NUMA nodes and the NIC's locality;
- pin EventLoop owners deliberately only after measuring the unpinned baseline;
- keep one connection on one owner for its lifetime; do not use affinity as a
  hidden connection-migration mechanism;
- place owner memory and network processing with NUMA locality where practical,
  but retain cross-node control/lifecycle progress in tests;
- record CPU frequency/power policy, SMT, processor groups and any isolated
  cores as part of the fixed-lab identity.

## NIC Queues, Interrupts, RSS/RPS/XPS

On Linux, inventory hardware RSS queues and IRQ affinity before changing RPS,
RFS or XPS. RPS is software steering and can add inter-processor interrupts;
XPS is ineffective when the device has only one transmit queue. Align queue/
IRQ and owner-loop placement only when the measured flow distribution supports
it. See the [Linux kernel scaling guide](https://docs.kernel.org/networking/scaling.html).

On Windows, inspect `Get-NetAdapterRss` before using `Set-NetAdapterRss`.
Queue count, processor range/profile and NUMA node are adapter-specific; changes
may restart the adapter. Microsoft recommends understanding RSS distribution
before changing individual fields. See [Set-NetAdapterRss](https://learn.microsoft.com/en-us/powershell/module/netadapter/set-netadapterrss?view=windowsserver2025-ps)
and [Windows Server NIC tuning](https://learn.microsoft.com/en-us/windows-server/networking/technologies/network-subsystem/net-sub-performance-tuning-nics).

## Socket And TCP Settings

- treat application pending-output limits separately from kernel socket buffers;
  increasing `SO_SNDBUF`/`SO_RCVBUF` never relaxes game-net-core admission;
- on Linux, record `net.core` maxima and `tcp_rmem`/`tcp_wmem`; explicitly
  setting a per-socket buffer interacts with TCP autotuning. See the
  [kernel IP sysctl reference](https://docs.kernel.org/networking/ip-sysctl.html);
- on Windows, buffer sizes are provider/host dependent and do not directly equal
  the TCP window. See [Winsock socket options](https://learn.microsoft.com/en-us/windows/win32/winsock/socket-options-and-ioctls-2);
- evaluate `TCP_NODELAY` for small latency-sensitive messages against bandwidth,
  packet rate and CPU; it disables Nagle coalescing and is not automatically a
  throughput improvement;
- record backlog, ephemeral-port, firewall, offload and interrupt-moderation
  settings when they differ from the host baseline.

## SO_REUSEPORT Boundary

Linux `SO_REUSEPORT` can distribute accepts across multiple listeners, but it
changes listener ownership/topology and must be enabled on every socket before
bind. It remains disabled unless the existing accept-topology contracts plus a
same-scenario fixed-lab comparison show benefit. See
[`socket(7)`](https://www.man7.org/linux/man-pages/man7/socket.7.html).

## Deployment Record

For every candidate record: CPU/NIC/NUMA identity, OS/kernel/driver/toolchain,
build preset, command line, affinity, queue/IRQ/RSS/RPS/XPS state, socket/sysctl
state, executable hash, commit, raw samples and rollback procedure. A setting
that improves median throughput but violates tail latency, recovery, memory or
shutdown is rejected.
