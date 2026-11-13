# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
from run_udp_peer_fuzz import corpus,verify_log
def main():
    good='Seed: 424242\nstat::number_of_executed_units: 20000\n'
    verify_log(good)
    for bad in [good.replace('20000','19999'),good.replace('424242','1'),good+good,good+'ERROR: AddressSanitizer',good+'runtime error: underflow']:
        try:verify_log(bad)
        except ValueError:pass
        else:raise AssertionError('invalid UDP peer fuzz evidence accepted')
    seeds=corpus();assert len(seeds)==36 and all(0<len(v)<=96 for v in seeds.values())
    root=Path(__file__).resolve().parents[2]
    workflow=(root/'.github/workflows/udp.yml').read_text(encoding='utf-8')
    for token in ['peer-fuzz:','gamenet_fuzz_udp_peer','run_udp_peer_fuzz.py --verify-output','GAMENET_ENABLE_ASAN_UBSAN=ON','GAMENET_BUILD_TESTING=OFF']:
        assert token in workflow,token
    print('UDP peer bounded fuzz count/seed/diagnostic and independent readback guards passed')
if __name__=='__main__':main()
