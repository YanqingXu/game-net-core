# Architecture Notes

Architecture decisions refine the active intents into implementation-ready
boundaries. They do not override the intents or the repository rules.

- [I/O Engine and Runtime Model ADR](io_engine_runtime_model_adr.md) records the
  ARCH-G1 owner, lifetime, semantic, compatibility, and test-map decisions.
- [ARCH-G1 baseline](../development/io_engine_baseline.md) records the
  exact-checkpoint Linux/epoll and Windows/IOCP behavior and performance sample
  against which IOE-R1 is compared.
- [Runtime Profile common-capability review](runtime_profile_common_capability_review.md)
  records the M5 second `NO-PROMOTION` decision against the independent gateway
  evidence.
- [Runtime Profile load-selection guide](runtime_profile_load_selection_guide.md)
  selects the non-installed Profile recipes from measured workload properties.
