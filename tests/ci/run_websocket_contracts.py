# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Exact optional WS contracts, independent interoperability and fuzz evidence."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading
import xml.etree.ElementTree as ET

CONTRACTS = {'unit.websocket.test_websocket_codec','contract.websocket.test_websocket_session','integration.websocket.test_websocket_tcp'}
def verify_inventory(document: dict) -> set[str]:
    tests=document.get('tests',[]);names=[t['name'] for t in tests]
    if len(names)!=136 or len(set(names))!=136: raise ValueError('WS inventory must be exactly 136 tests')
    selected=set()
    for t in tests:
        properties={p['name']:p['value'] for p in t.get('properties',[])}
        if 'websocket' in properties.get('LABELS',[]):
            if properties.get('DISABLED',False): raise ValueError('disabled WS contract')
            selected.add(t['name'])
    if selected!=CONTRACTS: raise ValueError('WS selection must be exactly three tests')
    return set(names)
def verify_results(root: ET.Element,expected: set[str]) -> None:
    cases=root.findall('.//testcase')
    if len(cases)!=len(expected) or {c.get('name') for c in cases}!=expected: raise ValueError('WS execution inventory mismatch')
    for c in cases:
        if c.get('status')!='run' or any(c.find(k) is not None for k in ['failure','error','skipped']): raise ValueError('failed/skipped/unexecuted WS case')
def verify_fuzz(text: str) -> None:
    if re.findall(r'stat::number_of_executed_units:\s+(\d+)',text)!=['20000'] or 'Seed: 424242\n' not in text: raise ValueError('exact bounded WS fuzz count/seed required')
    if re.search(r'ERROR:|runtime error:|DEADLYSIGNAL|SUMMARY:.*Sanitizer',text): raise ValueError('fuzz diagnostic present')

def interoperate(binary: Path,output: Path,environment: dict) -> dict:
    import websockets
    from websockets.sync.client import connect
    from websockets.exceptions import InvalidStatus
    if websockets.__version__!='15.0.1': raise ValueError('pinned real websockets 15.0.1 required')
    process=subprocess.Popen([str(binary),'0','8'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
        env=environment,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    lines=queue.Queue()
    def reader():
        with (output/'interop-server.log').open('wb') as log:
            for line in iter(process.stdout.readline,b''):
                log.write(line);log.flush();lines.put(line.decode('utf-8',errors='replace').strip())
    thread=threading.Thread(target=reader,daemon=True);thread.start()
    try:
        port=None
        for _ in range(100):
            line=lines.get(timeout=10)
            if line.startswith('PORT '): port=int(line.split()[1]);break
        if not port: raise ValueError('server did not publish its bound port')
        for generation in range(8):
            with connect(f'ws://127.0.0.1:{port}/echo',origin='http://localhost',compression=None,proxy=None,
                         open_timeout=3,close_timeout=3,max_size=1024*1024) as ws:
                text='hello \u20ac \U0001f600 '+str(generation)
                ws.send([text[:4],text[4:]]);assert ws.recv(timeout=3)==text
                payload=b'binary\0'+bytes(range(256))*273
                ws.send([payload[:123],payload[123:]]);assert ws.recv(timeout=3)==payload
                assert ws.ping(b'control-interoperation').wait(3)
                ws.close(1000,'done');assert ws.close_code==1000
        try:
            with connect(f'ws://127.0.0.1:{port}/denied',compression=None,proxy=None,open_timeout=3): pass
        except InvalidStatus as error:
            if error.response.status_code!=403: raise
        else: raise ValueError('handshake policy rejection was bypassed')
        result=process.wait(timeout=15)
        if result: raise ValueError(f'interoperability server exited {result}')
        thread.join(timeout=3)
        log=(output/'interop-server.log').read_text(encoding='utf-8',errors='replace')
        if re.search(r'ERROR:|runtime error:|WARNING: ThreadSanitizer|DEADLYSIGNAL',log): raise ValueError('interop sanitizer diagnostic')
        return {'implementation':'websockets','version':websockets.__version__,'generations':8,'messages':16,'pings':8,'policy_rejections':1,'status':'passed'}
    finally:
        if process.poll() is None:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill();process.wait(timeout=5)
        thread.join(timeout=3)

def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',choices=['release','asan','tsan','fuzz'],required=True)
    parser.add_argument('--build-dir',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();repo=Path(__file__).resolve().parents[2]
    build=args.build_dir.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();configuration='Release' if args.profile=='release' else 'Debug'
    if args.profile in {'asan','fuzz'}:env.update(ASAN_OPTIONS='halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if args.profile=='tsan':env['TSAN_OPTIONS']='halt_on_error=1'
    manifest={'schema':'gamenet.websocket_contracts.v1','profile':args.profile,'status':'failed',
        'workflow_sha':env.get('GITHUB_SHA'),'run_id':env.get('GITHUB_RUN_ID'),'run_attempt':env.get('GITHUB_RUN_ATTEMPT')}
    def execute(name,command,succeeds=True):
        (out/(name+'-command.json')).write_text(json.dumps(command,indent=2)+'\n',encoding='utf-8')
        print('Running '+name,flush=True)
        with (out/(name+'.log')).open('wb') as log:result=subprocess.run(command,cwd=repo,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
        text=(out/(name+'.log')).read_text(encoding='utf-8',errors='replace')
        if (result.returncode==0)!=succeeds:print(text[-12000:]);raise RuntimeError(f'{name}: unexpected exit {result.returncode}')
        return text
    def configure(directory,enabled):
        command=['cmake','-S',str(repo),'-B',str(directory),'-DCMAKE_BUILD_TYPE='+configuration,
            '-DGAMENET_ENABLE_WEBSOCKET='+('ON' if enabled else 'OFF'),'-DGAMENET_ENABLE_TLS=OFF','-DGAMENET_ENABLE_UDP=OFF',
            '-DGAMENET_BUILD_TESTING=ON','-DGAMENET_BUILD_BENCHMARKS=OFF','-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON']
        command+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        if enabled and args.profile in {'asan','fuzz'}:command+=['-DGAMENET_ENABLE_ASAN_UBSAN=ON']
        if enabled and args.profile=='tsan':command+=['-DGAMENET_ENABLE_TSAN=ON']
        if enabled and args.profile=='fuzz':command+=['-DGAMENET_BUILD_FUZZING=ON']
        return command
    def consumer(name,prefix,component,succeeds=True):
        directory=build/name
        command=['cmake','-S',str(repo/'tests/cmake/websocket_install_consumer'),'-B',str(directory),
            '-DCMAKE_BUILD_TYPE='+configuration,'-DCMAKE_PREFIX_PATH='+str(prefix),'-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON',
            '-DREQUEST_WEBSOCKET='+('ON' if component else 'OFF')]
        command+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        text=execute(name+'-configure',command,succeeds)
        if not succeeds:
            if 'Required component websocket is unavailable' not in text:raise ValueError('OFF rejection failed for wrong reason')
            return
        execute(name+'-build',['cmake','--build',str(directory),'--config',configuration,'--parallel','2'])
        text=execute(name+'-test',['ctest','--test-dir',str(directory),'-C',configuration,'--no-tests=error','--output-on-failure'])
        if '100% tests passed' not in text:raise ValueError('installed consumer did not execute')
    try:
        manifest['commit_sha']=execute('commit',['git','rev-parse','HEAD']).strip()
        manifest['working_tree']=execute('worktree',['git','status','--porcelain=v1'])
        if manifest['workflow_sha'] and (manifest['workflow_sha']!=manifest['commit_sha'] or manifest['working_tree']):raise ValueError('exact clean workflow checkout required')
        execute('configure',configure(build,True))
        for path in (build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
            match=re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)',path.read_text(encoding='utf-8'))
            if match:env['PATH']=str(Path(match[1]).parent)+os.pathsep+env.get('PATH','');break
        if args.profile=='fuzz':
            execute('build',['cmake','--build',str(build),'--parallel','2','--target','gamenet_fuzz_websocket','gamenet_fuzz_websocket_session'])
            corpus=out/'corpus';corpus.mkdir(exist_ok=True)
            # Separate protocol seeds: arbitrary bytes plus canonical Upgrade/masked frame.
            seeds={'upgrade':b'GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WS-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WS-Version: 13\r\n\r\n',
                   'text':b'\x81\x85\0\0\0\0hello','fragment':b'\x01\x81\0\0\0\0a\x80\x81\0\0\0\0b','ping':b'\x89\x80\0\0\0\0','bad-utf8':b'\x81\x82\0\0\0\0\xc0\x80'}
            for name,value in seeds.items():(corpus/name).write_bytes(value)
            binary=build/'tests/fuzz/gamenet_fuzz_websocket'
            log=execute('fuzzer',[str(binary),str(corpus),'-runs=20000','-seed=424242','-max_len=2048','-timeout=5','-rss_limit_mb=512','-print_final_stats=1','-artifact_prefix='+str(out)+'/'])
            verify_fuzz(log);manifest['executed_units']=20000
            execute('session-fuzz',[sys.executable,str(repo/'tests/ci/run_websocket_session_fuzz.py'),'--build-dir',str(build),'--output',str(out/'session')])
            manifest['session_fuzz']=json.loads((out/'session/manifest.json').read_text(encoding='utf-8'))
            if manifest['session_fuzz']['status']!='passed' or manifest['session_fuzz']['executed_units']!=20000:raise ValueError('missing session fuzz evidence')
        else:
            inventory=verify_inventory(json.loads(execute('inventory',['ctest','--test-dir',str(build),'-C',configuration,'--show-only=json-v1'])))
            api=json.loads((repo/'api/public_api_manifest.json').read_text(encoding='utf-8'))
            installed=['gamenet_'+n.split('::')[1] for group in api['targets'].values() for n in group if n not in {'GameNet::udp','GameNet::tls','GameNet::rpc_tls','GameNet::websocket_tls'}]
            targets=['gnt_'+n.split('.')[0]+'_websocket_'+hashlib.sha1(n.encode()).hexdigest()[:10] for n in sorted(CONTRACTS)]
            command=['cmake','--build',str(build),'--config',configuration,'--parallel','2']
            if args.profile!='release':command+=['--target',*installed,*targets,'gamenet_websocket_echo_server']
            execute('build',command)
            command=['ctest','--test-dir',str(build),'-C',configuration,'--output-on-failure','--timeout','60','--no-tests=error','--output-junit',str(out/'ctest-junit.xml')]
            if args.profile!='release':command+=['-L','^websocket$']
            execute('ctest',command);expected=inventory if args.profile=='release' else CONTRACTS
            verify_results(ET.parse(out/'ctest-junit.xml').getroot(),expected);manifest['executed_tests']=len(expected)
            binary=build/'examples'/('' if os.name!='nt' else configuration)/('gamenet_websocket_echo_server'+('.exe' if os.name=='nt' else ''))
            manifest['interop']=interoperate(binary,out,env)
            prefix=build/'prefix';execute('install',['cmake','--install',str(build),'--config',configuration,'--prefix',str(prefix)])
            consumer('ws-consumer',prefix,True);consumer('core-only-consumer',prefix,False)
            if args.profile=='release':
                off=build/'off';execute('off-configure',configure(off,False))
                execute('off-build',['cmake','--build',str(off),'--config',configuration,'--parallel','2','--target',*[t for t in installed if t!='gamenet_websocket']])
                off_prefix=off/'prefix';execute('off-install',['cmake','--install',str(off),'--config',configuration,'--prefix',str(off_prefix)])
                if (off_prefix/'include/gamenet/websocket').exists() or list(off_prefix.rglob('GameNetWebSocketTargets.cmake')):raise ValueError('OFF leaked WS surface')
                consumer('off-core-consumer',off_prefix,False);consumer('off-ws-rejection',off_prefix,True,False);manifest['off_isolation']='passed'
        manifest['status']='passed'
    finally:
        manifest['files']={p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file() and p!=out/'manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
