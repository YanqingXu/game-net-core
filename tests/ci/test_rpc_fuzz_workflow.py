# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
from run_rpc_fuzz import KINDS, corpus, verify_log

def main() -> None:
    valid = "INFO: Seed: 424242\nstat::number_of_executed_units: 20000\n"
    verify_log(valid, 20000, 424242)
    for bad in ["", valid.replace("20000", "19999"), valid.replace("424242", "1"), valid + valid,
                *[valid + x for x in ["ERROR: AddressSanitizer", "runtime error: overflow", "DEADLYSIGNAL"]]]:
        try: verify_log(bad, 20000, 424242)
        except ValueError: pass
        else: raise AssertionError("invalid fuzz execution evidence accepted")
    assert set(KINDS) == {"codec", "channel"}
    assert all(corpus(kind) and all(corpus(kind).values()) for kind in KINDS)
    root = Path(__file__).resolve().parents[2]
    workflow = (root / ".github/workflows/rpc-fuzz.yml").read_text(encoding="utf-8")
    for text in ["run_rpc_fuzz.py", "--runs 20000", "--seed 424242", "if: always()", "if-no-files-found: error",
                 "gamenet_fuzz_rpc_codec gamenet_fuzz_rpc_channel", "GAMENET_ENABLE_ASAN_UBSAN=ON"]:
        assert text in workflow, text
    assert "continue-on-error" not in workflow
    print("RPC bounded-fuzz evidence rejects missing/count/seed/sanitizer drift")

if __name__ == "__main__": main()
