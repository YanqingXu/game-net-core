# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import copy
from pathlib import Path
import xml.etree.ElementTree as ET
from run_websocket_contracts import CONTRACTS,verify_inventory,verify_results,verify_fuzz
def rejects(function,value):
    try:function(value)
    except ValueError:return
    raise AssertionError('invalid WS evidence accepted')
def main():
    inventory={'tests':[{'name':n,'properties':[{'name':'LABELS','value':['websocket'] if n in CONTRACTS else ['baseline']}]} for n in [*[f'baseline.test.{n}' for n in range(133)],*sorted(CONTRACTS)]]}
    verify_inventory(inventory)
    bad=copy.deepcopy(inventory);bad['tests'].pop();rejects(verify_inventory,bad)
    bad=copy.deepcopy(inventory);bad['tests'][-1]['properties'].append({'name':'DISABLED','value':True});rejects(verify_inventory,bad)
    suite=ET.Element('testsuite')
    for name in CONTRACTS:ET.SubElement(suite,'testcase',{'name':name,'status':'run'})
    verify_results(suite,CONTRACTS)
    for kind in ['failure','error','skipped']:
        bad=copy.deepcopy(suite);ET.SubElement(bad[0],kind);rejects(lambda x:verify_results(x,CONTRACTS),bad)
    valid='INFO: Seed: 424242\nstat::number_of_executed_units: 20000\n';verify_fuzz(valid)
    for bad in ['',valid.replace('20000','19999'),valid.replace('424242','1'),valid+'ERROR: AddressSanitizer',valid+valid]:rejects(verify_fuzz,bad)
    root=Path(__file__).resolve().parents[2]
    workflow=(root/'.github/workflows/websocket.yml').read_text(encoding='utf-8')
    for fragment in ['linux-release','linux-asan-ubsan','linux-tsan','windows-release','windows-asan','linux-fuzz','websockets==15.0.1','--with-websocket','run_websocket_contracts.py','if: always()','if-no-files-found: error']:
        assert fragment in workflow,fragment
    assert 'continue-on-error' not in workflow
    assert 'option(GAMENET_ENABLE_WEBSOCKET "Build bounded WS server support" OFF)' in (root/'CMakeLists.txt').read_text(encoding='utf-8')
    print('WS exact inventory/JUnit/fuzz evidence rejects missing, disabled, skipped and count/seed drift')
if __name__=='__main__':main()
