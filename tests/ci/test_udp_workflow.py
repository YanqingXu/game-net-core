# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import copy
from pathlib import Path
import xml.etree.ElementTree as ET
from run_udp_contracts import CONTRACTS,INTEROP,PEER_INTEROP,verify_inventory,verify_results,verify_interop,verify_peer_interop

def rejects(function,value):
    try:function(value)
    except ValueError:return
    raise AssertionError('invalid UDP evidence accepted')
def main():
    inventory={'tests':[{'name':n,'properties':[{'name':'LABELS','value':['udp'] if n in CONTRACTS else ['baseline']}]} for n in [*[f'baseline.test.{i}' for i in range(133)],*sorted(CONTRACTS)]]}
    verify_inventory(inventory)
    bad=copy.deepcopy(inventory);bad['tests'].pop();rejects(verify_inventory,bad)
    bad=copy.deepcopy(inventory);bad['tests'][-1]['properties'].append({'name':'DISABLED','value':True});rejects(verify_inventory,bad)
    bad=copy.deepcopy(inventory);bad['tests'][-1]['name']=bad['tests'][0]['name'];rejects(verify_inventory,bad)
    root=ET.Element('testsuite')
    for name in CONTRACTS:ET.SubElement(root,'testcase',{'name':name,'status':'run'})
    verify_results(root,CONTRACTS)
    for kind in ['failure','error','skipped']:
        bad=copy.deepcopy(root);ET.SubElement(bad[0],kind);rejects(lambda v:verify_results(v,CONTRACTS),bad)
    bad=copy.deepcopy(root);bad[0].set('status','notrun');rejects(lambda v:verify_results(v,CONTRACTS),bad)
    verify_interop(INTEROP)
    verify_peer_interop(PEER_INTEROP)
    for key,value in [('families',[4]),('datagrams',63),('rotations',0),('unknown_sources',0),('status','failed')]:
        bad=copy.deepcopy(PEER_INTEROP);bad[key]=value;rejects(verify_peer_interop,bad)
    for key,value in [('families',[4]),('datagrams',47),('empty_datagrams',0),('truncated_datagrams',0),('generations',7),('status','failed')]:
        bad=copy.deepcopy(INTEROP);bad[key]=value;rejects(verify_interop,bad)
    repo=Path(__file__).resolve().parents[2];workflow=(repo/'.github/workflows/udp.yml').read_text(encoding='utf-8')
    for token in ['linux-release','linux-asan-ubsan','linux-tsan','windows-release','windows-asan','--with-udp','run_udp_contracts.py','run_udp_peer_fuzz.py','gamenet_fuzz_udp_peer','if: always()','if-no-files-found: error']:
        assert token in workflow,token
    assert 'continue-on-error' not in workflow
    assert 'option(GAMENET_ENABLE_UDP "Build bounded UDP transport" OFF)' in (repo/'CMakeLists.txt').read_text(encoding='utf-8')
    driver=(repo/'tests/ci/run_udp_contracts.py').read_text(encoding='utf-8')
    for token in ['CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON','off-udp-rejection','--no-tests=error','verify_interop','binary_sha256',"manifest['files']"]:assert token in driver
    for token in ['adapter-consumer','off-adapter-rejection','GameNetUdpTransportTargets.cmake']:assert token in driver
    peer=(repo/'include/gamenet/udp/UdpPeer.h').read_text(encoding='utf-8')
    assert 'gamenet/transport/' not in peer and 'gamenet/udp_transport/' not in peer
    udp_cmake=(repo/'src/udp/CMakeLists.txt').read_text()
    assert 'GameNet::transport' not in udp_cmake and 'GameNet::udp_transport' not in udp_cmake
    print('UDP exact inventory, actual execution, dual-family peers and default-OFF guards passed')
if __name__=='__main__':main()
