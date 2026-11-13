# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import copy,json
from pathlib import Path
import xml.etree.ElementTree as ET
from run_kcp_contracts import CONTRACTS,UDP_CONTRACTS,INTEROP,verify_inventory,verify_results,verify_interop
def rejects(f,v):
    try:f(v)
    except ValueError:return
    raise AssertionError('invalid KCP evidence accepted')
def main():
    names=[*[f'baseline.test.{i}' for i in range(133)],*sorted(UDP_CONTRACTS|CONTRACTS)]
    inventory={'tests':[{'name':n,'properties':[{'name':'LABELS','value':['kcp'] if n in CONTRACTS else ['other']}]} for n in names]}
    verify_inventory(inventory)
    bad=copy.deepcopy(inventory);bad['tests'].pop();rejects(verify_inventory,bad)
    bad=copy.deepcopy(inventory);bad['tests'][-1]['name']=bad['tests'][0]['name'];rejects(verify_inventory,bad)
    bad=copy.deepcopy(inventory)
    for t in bad['tests']:
        if t['name'] in CONTRACTS:t['properties'].append({'name':'DISABLED','value':True});break
    rejects(verify_inventory,bad)
    root=ET.Element('testsuite')
    for name in CONTRACTS:ET.SubElement(root,'testcase',{'name':name,'status':'run'})
    verify_results(root,CONTRACTS)
    for kind in ['failure','error','skipped']:
        bad=copy.deepcopy(root);ET.SubElement(bad[0],kind);rejects(lambda v:verify_results(v,CONTRACTS),bad)
    bad=copy.deepcopy(root);bad[0].set('status','notrun');rejects(lambda v:verify_results(v,CONTRACTS),bad)
    verify_interop(INTEROP)
    for key,value in [('families',[4]),('messages',7),('data_loss_directions',0),('ack_loss_directions',0),('duplicate_directions',0),('reorder_directions',0),('zero_residue',False),('status','failed')]:
        bad=copy.deepcopy(INTEROP);bad[key]=value;rejects(verify_interop,bad)
    repo=Path(__file__).resolve().parents[2];workflow=(repo/'.github/workflows/kcp.yml').read_text(encoding='utf-8')
    for token in ['linux-release','linux-asan-ubsan','linux-tsan','windows-release','windows-asan','--with-kcp','run_kcp_contracts.py','--verify-output','if: always()','if-no-files-found: error']:
        assert token in workflow,token
    assert 'continue-on-error' not in workflow
    assert 'option(GAMENET_ENABLE_KCP "Build bounded standard KCP messages" OFF)' in (repo/'CMakeLists.txt').read_text()
    cmake=(repo/'src/kcp/CMakeLists.txt').read_text()
    assert 'PUBLIC GameNet::udp' in cmake and 'GameNet::transport' not in cmake and '/EHsc-' in cmake
    upstream=json.loads((repo/'third_party/kcp/upstream.json').read_text());assert upstream['modified'] is False
    reference=(repo/'tests/integration/kcp/upstream_reference.cc').read_text()
    assert '#include "gamenet/' not in reference
    print('KCP inventory, execution, standard peer, dependency and platform guards passed')
if __name__=='__main__':main()
