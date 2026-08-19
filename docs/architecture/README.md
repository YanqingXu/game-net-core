# Architecture Notes

Architecture decisions refine the active intents into implementation-ready
boundaries. They do not override the intents or the repository rules.

- [I/O Engine and Runtime Model ADR](io_engine_runtime_model_adr.md) records the
  ARCH-G1 owner, lifetime, semantic, compatibility, and test-map decisions.
- [ARCH-G1 baseline](../development/io_engine_baseline.md) records the
  exact-checkpoint Linux/epoll and Windows/IOCP behavior and performance sample
  against which IOE-R1 is compared.
