# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
from run_kcp_fuzz import corpus,verify_log
def main():
    good='Seed: 424242\nstat::number_of_executed_units: 20000\n';verify_log(good)
    for bad in [good.replace('20000','19999'),good.replace('424242','1'),good+good,good+'ERROR: AddressSanitizer',good+'runtime error: overflow']:
        try:verify_log(bad)
        except ValueError:pass
        else:raise AssertionError('invalid KCP fuzz evidence accepted')
    assert len(corpus())==36 and all(0<len(v)<=96 for v in corpus().values())
    repo=Path(__file__).resolve().parents[2];workflow=(repo/'.github/workflows/kcp.yml').read_text()
    for kind in ('protocol','session'):
        for token in ['gamenet_fuzz_kcp_'+kind,'--kind '+kind,'--verify-output out/kcp-'+kind+'-fuzz']:assert token in workflow
    for token in ['GAMENET_ENABLE_ASAN_UBSAN=ON','GAMENET_BUILD_TESTING=OFF']:assert token in workflow
    print('KCP fuzz count/seed/sanitizer and independent readback guards passed')
if __name__=='__main__':main()
