# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import copy
import json
import tempfile
from pathlib import Path
import xml.etree.ElementTree as ET
from run_websocket_tls_contracts import CONTRACTS,verify_inventory,verify_results,verify_interop,verify_interop_rounds
from wss_timeout_repro import verify_timeout_proof
from wss_interop_diagnostics import verify_peer_diagnostics

def rejects(fn,value):
    try:fn(value)
    except ValueError:return
    raise AssertionError('invalid WSS evidence accepted')

def diagnostic_guards():
    clients=[];servers=[]
    for generation in range(8):
        clients.extend([{'event':'connection','phase':'connected','generation':generation,'local':['127.0.0.1',10000+generation]},
            {'event':'connection','phase':'closed','generation':generation,'close_code':1000,'close_reason':'done',
             'receive_exception':'None','parser_exception':'None','close_sent':"Close(code=1000, reason='done')",
             'close_received':"Close(code=1000, reason='done')",'trace':[{'message':'> CLOSE 1000 (OK) done'},{'message':'< CLOSE 1000 (OK) done'}]}])
        servers.append({'event':'closed','peer_port':10000+generation,'terminals':1,'ws_sent_close':1,'ws_received_close':1,
            'tls_sent_notify':1,'tls_received_notify':1,'tcp_pending':0,'ws_controls':0,'ws_input':0,'tls_pending':0,'tls_input':0,
            'tls_output':0,'tls_accepted':16,'tls_encrypted':16,'tls_discarded':0,'ws_terminal':1,'tls_terminal':1,
            'ws_fragmented':0,'ws_pending_controls':0,'ws_continuation':0,'tls_pending_writes':0,'ws_enqueued':8,'ws_admitted':8,'ws_discarded':0})
    clients.extend([{'event':'certificate-rejected'} for _ in range(3)]);clients.append({'event':'policy-rejected','status':403})
    with tempfile.TemporaryDirectory() as temporary:
        folder=Path(temporary)
        def verify(values):
            peers,states=values
            (folder/'interop-client.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in peers))
            (folder/'interop-server.log').write_text(''.join('TRACE '+json.dumps(x)+'\n' for x in states))
            verify_peer_diagnostics(folder)
        verify((clients,servers))
        for field,value in [('close_code',1006),('receive_exception','TimeoutError'),('trace',[])]:
            bad=copy.deepcopy(clients);bad[1][field]=value;rejects(verify,(bad,servers))
        for field,value in [('terminals',2),('tls_received_notify',0),('tcp_pending',1),('tls_pending',1),('tls_encrypted',0)]:
            bad=copy.deepcopy(servers);bad[0][field]=value;rejects(verify,(clients,bad))
        rejects(verify,(clients[:-1],servers));rejects(verify,(clients,servers[:-1]))
        bad=copy.deepcopy(clients);bad[3]=copy.deepcopy(bad[1]);rejects(verify,(bad,servers))
def main():
    diagnostic_guards()
    names=[*[f'baseline.{i}' for i in range(143-len(CONTRACTS))],*sorted(CONTRACTS)]
    inventory={'tests':[{'name':n,'properties':[]} for n in names]};verify_inventory(inventory)
    bad=copy.deepcopy(inventory);bad['tests'].pop();rejects(verify_inventory,bad)
    bad=copy.deepcopy(inventory);bad['tests'][-1]['properties']=[{'name':'DISABLED','value':True}];rejects(verify_inventory,bad)
    suite=ET.Element('testsuite')
    for name in CONTRACTS:ET.SubElement(suite,'testcase',{'name':name,'status':'run'})
    verify_results(suite,CONTRACTS)
    for kind in ['failure','error','skipped']:
        bad=copy.deepcopy(suite);ET.SubElement(bad[0],kind);rejects(lambda x:verify_results(x,CONTRACTS),bad)
    good={'implementation':'websockets','client_api':'asyncio','version':'15.0.1','generations':8,'messages':16,'pings':8,'policy_rejections':1,'certificate_rejections':3,'graceful_closes':9,'truncated_closes':1,'status':'passed'}
    verify_interop(good)
    for field in ['generations','certificate_rejections','graceful_closes','status']:
        bad=copy.deepcopy(good);bad[field]=0;rejects(verify_interop,bad)
    verify_interop_rounds([good,good,good])
    rejects(verify_interop_rounds,[good,good])
    bad=copy.deepcopy(good);bad['client_api']='sync';rejects(verify_interop,bad)
    rejects(verify_interop_rounds,[good,good,bad])
    proof={'status':'legacy_false_timeout_reproduced','timeout_at_header':None,'new_timeout_seconds':3,
           'exception_type':'TimeoutError','failed_before_body_release':True,'worker_errors':[],'elapsed_ns':100000}
    verify_timeout_proof(proof)
    for field,value in [('timeout_at_header',3),('exception_type','EOFError'),('failed_before_body_release',False),('worker_errors',['unretired']),('elapsed_ns',0)]:
        bad=copy.deepcopy(proof);bad[field]=value;rejects(verify_timeout_proof,bad)
    root=Path(__file__).resolve().parents[2];workflow=(root/'.github/workflows/websocket-tls.yml').read_text()
    for token in ['linux-release','linux-asan-ubsan','linux-tsan','windows-release','windows-asan','websockets==15.0.1','--with-tls --with-websocket','run_websocket_tls_contracts.py','if: always()','if-no-files-found: error']:
        assert token in workflow,token
    assert 'continue-on-error' not in workflow
    config=(root/'cmake/GameNetCoreConfig.cmake.in').read_text()
    assert 'Required component websocket_tls is unavailable' in config
    print('WSS exact inventories/JUnit/interop guards and five mandatory real TLS lanes passed')
if __name__=='__main__':main()
