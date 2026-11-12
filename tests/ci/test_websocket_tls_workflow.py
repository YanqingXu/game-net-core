# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import copy
from pathlib import Path
import xml.etree.ElementTree as ET
from run_websocket_tls_contracts import CONTRACTS,verify_inventory,verify_results,verify_interop

def rejects(fn,value):
    try:fn(value)
    except ValueError:return
    raise AssertionError('invalid WSS evidence accepted')
def main():
    names=[*[f'baseline.{i}' for i in range(143-len(CONTRACTS))],*sorted(CONTRACTS)]
    inventory={'tests':[{'name':n,'properties':[]} for n in names]};verify_inventory(inventory)
    bad=copy.deepcopy(inventory);bad['tests'].pop();rejects(verify_inventory,bad)
    bad=copy.deepcopy(inventory);bad['tests'][-1]['properties']=[{'name':'DISABLED','value':True}];rejects(verify_inventory,bad)
    suite=ET.Element('testsuite')
    for name in CONTRACTS:ET.SubElement(suite,'testcase',{'name':name,'status':'run'})
    verify_results(suite,CONTRACTS)
    for kind in ['failure','error','skipped']:
        bad=copy.deepcopy(suite);ET.SubElement(bad[0],kind);rejects(lambda x:verify_results(x,CONTRACTS),bad)
    good={'implementation':'websockets','version':'15.0.1','generations':8,'messages':16,'pings':8,'policy_rejections':1,'certificate_rejections':3,'graceful_closes':1,'truncated_closes':8,'status':'passed'}
    verify_interop(good)
    for field in ['generations','certificate_rejections','graceful_closes','status']:
        bad=copy.deepcopy(good);bad[field]=0;rejects(verify_interop,bad)
    root=Path(__file__).resolve().parents[2];workflow=(root/'.github/workflows/websocket-tls.yml').read_text()
    for token in ['linux-release','linux-asan-ubsan','linux-tsan','windows-release','windows-asan','websockets==15.0.1','--with-tls --with-websocket','run_websocket_tls_contracts.py','if: always()','if-no-files-found: error']:
        assert token in workflow,token
    assert 'continue-on-error' not in workflow
    config=(root/'cmake/GameNetCoreConfig.cmake.in').read_text()
    assert 'Required component websocket_tls is unavailable' in config
    print('WSS exact inventories/JUnit/interop guards and five mandatory real TLS lanes passed')
if __name__=='__main__':main()
