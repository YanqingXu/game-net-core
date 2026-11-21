# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import copy,json,subprocess
from pathlib import Path
import xml.etree.ElementTree as ET
from run_kcp_contracts import CONTRACTS,UDP_CONTRACTS,INTEROP,verify_inventory,verify_results,verify_interop
from kcp_interop import verify_settled_reset
from kcp_game_layers import TEST as GAME_TEST, EXPECTED, verify_game_layers
from test_workflow_jobs import job_block, step_block


def verify_manual_soak(workflow):
    dispatch = workflow.split('  workflow_dispatch:\n', 1)[1].split('  push:\n', 1)[0]
    assert '      c4_soak:\n' in dispatch
    assert '        type: boolean\n' in dispatch and '        default: false\n' in dispatch
    assert 'group: kcp-${{ inputs.c4_soak && github.run_id || github.ref }}' in workflow
    for job in ('clang-tsan-full', 'kcp', 'fuzz'):
        assert "if: github.event_name != 'workflow_dispatch' || !inputs.c4_soak" in job_block(workflow, job)

    soak = job_block(workflow, 'c4-soak')
    assert "if: github.event_name == 'workflow_dispatch' && inputs.c4_soak" in soak
    assert 'runs-on: ${{ matrix.os }}' in soak and 'os: [ubuntu-24.04, windows-latest]' in soak
    assert 'fail-fast: false' in soak
    assert 'ref: ${{ github.sha }}' in step_block(soak, 'Checkout the dispatched commit')
    run = 'run: python tools/run_kcp_capacity_endurance.py'
    evidence = 'out/kcp-c4-soak'
    assert f'{run} --profile release --mode soak --build-dir build-kcp-soak --output {evidence}' in soak
    assert f'{run} --verify-output {evidence}' in step_block(soak, 'Read back raw endurance evidence')
    upload = step_block(soak, 'Retain raw endurance evidence')
    assert 'if: always()' in upload and 'uses: actions/upload-artifact@' in upload
    assert 'name: kcp-c4-soak-${{ matrix.os }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}' in upload
    assert f'path: {evidence}/' in upload and 'if-no-files-found: error' in upload


def rejects(f,v):
    try:f(v)
    except ValueError:return
    raise AssertionError('invalid KCP evidence accepted')
def main():
    class Process:
        def __init__(self,code):self.code=code
        def wait(self,timeout):
            if self.code is None:raise subprocess.TimeoutExpired('test-peer',timeout)
            return self.code
    verify_settled_reset(Process(0))
    for code in [None,1,-1]:rejects(verify_settled_reset,Process(code))
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
    case=next(t for t in root if t.get('name')==GAME_TEST)
    ET.SubElement(case,'system-out').text='\n'.join('KCP_C3 '+' '.join(f'{k}={v}' for k,v in dict(family=family,**EXPECTED).items()) for family in [4,6])
    verify_game_layers(root)
    bad=copy.deepcopy(root);next(t for t in bad if t.get('name')==GAME_TEST).find('system-out').text='';rejects(verify_game_layers,bad)
    bad=copy.deepcopy(root);next(t for t in bad if t.get('name')==GAME_TEST).find('system-out').text=case.findtext('system-out').replace('family=6','family=4');rejects(verify_game_layers,bad)
    bad=copy.deepcopy(inventory)
    next(t for t in bad['tests'] if t['name']==GAME_TEST)['properties'][0]['value']=['other']
    rejects(verify_inventory,bad)
    for kind in ['failure','error','skipped']:
        bad=copy.deepcopy(root);ET.SubElement(bad[0],kind);rejects(lambda v:verify_results(v,CONTRACTS),bad)
    bad=copy.deepcopy(root);bad[0].set('status','notrun');rejects(lambda v:verify_results(v,CONTRACTS),bad)
    verify_interop(INTEROP)
    for key,value in [('families',[4]),('messages',7),('data_loss_directions',0),('ack_loss_directions',0),('duplicate_directions',0),('reorder_directions',0),('zero_residue',False),('status','failed')]:
        bad=copy.deepcopy(INTEROP);bad[key]=value;rejects(verify_interop,bad)
    repo=Path(__file__).resolve().parents[2];workflow=(repo/'.github/workflows/kcp.yml').read_text(encoding='utf-8')
    verify_manual_soak(workflow)
    for token in ['linux-release','linux-asan-ubsan','linux-tsan','windows-release','windows-asan','--with-kcp','run_kcp_contracts.py','--verify-output','if: always()','if-no-files-found: error',
                  'clang-tsan-full:','clang++-18','libclang-rt-18-dev','run_clang_tsan_gate.py','test_clang_tsan_gate.py','--verify-output out/clang-tsan-evidence','include-hidden-files: true']:
        assert token in workflow,token
    assert 'python tests/ci/kcp_examples.py --self-test' in workflow
    for token in ['test_kcp_capacity_endurance.py', 'test_kcp_validation_contract.py',
                  'tools/run_kcp_capacity_endurance.py --profile', '--compiler clang++-18 --build-dir build-clang-tsan',
                  '--verify-output out/kcp-c4-evidence', '--verify-output out/kcp-c4-clang-tsan']:
        assert token in workflow, token
    assert 'PUBLIC GameNet::kcp GameNet::transport' in (repo/'src/kcp_transport/CMakeLists.txt').read_text()
    assert 'continue-on-error' not in workflow
    assert 'option(GAMENET_ENABLE_KCP "Build bounded standard KCP messages" OFF)' in (repo/'CMakeLists.txt').read_text()
    cmake=(repo/'src/kcp/CMakeLists.txt').read_text()
    assert 'PUBLIC GameNet::udp' in cmake and 'GameNet::transport' not in cmake and '/EHsc-' in cmake
    upstream=json.loads((repo/'third_party/kcp/upstream.json').read_text());assert upstream['modified'] is False
    reference=(repo/'tests/integration/kcp/upstream_reference.cc').read_text()
    assert '#include "gamenet/' not in reference
    print('KCP inventory, execution, standard peer, dependency and platform guards passed')
if __name__=='__main__':main()
