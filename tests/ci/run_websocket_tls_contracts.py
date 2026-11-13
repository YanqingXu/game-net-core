# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Exact optional WSS contracts, independent peers and installed isolation."""
from __future__ import annotations
import argparse,asyncio,hashlib,json,os,queue,re,socket,ssl,subprocess,threading
from pathlib import Path
import xml.etree.ElementTree as ET
from run_websocket_contracts import CONTRACTS as WS,verify_results
from run_tls_contracts import CONTRACTS as TLS
from wss_interop_diagnostics import ClientTrace,verify_peer_diagnostics
from wss_async_peer import exchange
from wss_timeout_repro import reproduce
WSS={'contract.websocket_tls.test_websocket_tls_session','integration.websocket_tls.test_websocket_tls_tcp'}
CONTRACTS=WS|TLS|WSS

def verify_inventory(document):
    tests=document.get('tests',[]);names=[t['name'] for t in tests]
    if len(names)!=143 or len(set(names))!=143:raise ValueError('WSS inventory must be exactly 143')
    selected=set()
    for t in tests:
        props={p['name']:p['value'] for p in t.get('properties',[])}
        if t['name'] in CONTRACTS:
            if props.get('DISABLED',False):raise ValueError('disabled composition contract')
            selected.add(t['name'])
    if selected!=CONTRACTS:raise ValueError('missing WS/TLS/WSS contracts')
    return set(names)

def verify_interop(data):
    if data!={'implementation':'websockets','client_api':'asyncio','version':'15.0.1','generations':8,'messages':16,'pings':8,'policy_rejections':1,'certificate_rejections':3,'graceful_closes':9,'truncated_closes':1,'status':'passed'}:
        raise ValueError('incomplete independent WSS evidence')

def verify_interop_rounds(rounds):
    if not isinstance(rounds,list) or len(rounds)!=3:raise ValueError('three independent WSS rounds required')
    for result in rounds:verify_interop(result)

def interoperate(binary,certs,out,env):
    import websockets
    from websockets.asyncio.client import connect
    from websockets.exceptions import InvalidStatus
    from websockets.client import ClientProtocol
    from websockets.uri import parse_uri
    from websockets.protocol import OPEN
    if websockets.__version__!='15.0.1':raise ValueError('pinned websockets 15.0.1 required')
    context=ssl.create_default_context(cafile=str(certs/'ca.pem'))
    wrong=ssl.create_default_context(cafile=str(certs/'other-ca.pem'))
    trace=ClientTrace(out/'interop-client.jsonl')
    async def reject_peer(port,trust,hostname):
        try:
            async with connect(f'wss://127.0.0.1:{port}/echo',ssl=trust,server_hostname=hostname,proxy=None,compression=None,open_timeout=3):pass
        except ssl.SSLCertVerificationError as error:
            trace.write('certificate-rejected',hostname=hostname,exception=repr(error),verify_code=error.verify_code)
            return
        raise ValueError('certificate rejection was bypassed')
    def server(label,certificate,exercise):
        process=subprocess.Popen([str(binary),str(certs/certificate),str(certs/'key.pem'),'0','8'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        lines=queue.Queue()
        def reader():
            with (out/(label+'-server.log')).open('wb') as log:
                for line in iter(process.stdout.readline,b''):
                    log.write(line);log.flush();lines.put(line.decode('utf-8',errors='replace').strip())
        thread=threading.Thread(target=reader,daemon=True);thread.start()
        try:
            port=None
            for _ in range(100):
                line=lines.get(timeout=10)
                if line.startswith('PORT '):port=int(line.split()[1]);break
            if not port:raise ValueError('WSS peer did not publish port')
            exercise(port,lines)
            if process.wait(timeout=15):raise ValueError('WSS peer process failed')
            thread.join(timeout=3)
            text=(out/(label+'-server.log')).read_text(encoding='utf-8',errors='replace')
            if re.search(r'ERROR:|runtime error:|WARNING: ThreadSanitizer|DEADLYSIGNAL',text):raise ValueError('WSS peer sanitizer failure')
            return text
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
            thread.join(timeout=3)
    def rejected(port,trust,hostname='localhost'):
        asyncio.run(reject_peer(port,trust,hostname))
    def closed(lines,expected=None):
        while True:
            line=lines.get(timeout=5)
            if line.startswith('CLOSED '):
                if expected and line!=expected:raise ValueError('unexpected layered close: '+line)
                return line
    def healthy(port,lines):
        for generation in range(8):
            asyncio.run(exchange(f'wss://127.0.0.1:{port}/echo',context,generation,trace))
            closed(lines,'CLOSED 1 1 1')
        # Independent stdlib TLS peer explicitly completes both shutdown layers.
        for graceful in [True,False]:
          with socket.create_connection(('127.0.0.1',port),timeout=3) as raw:
            with context.wrap_socket(raw,server_hostname='localhost') as peer:
                protocol=ClientProtocol(parse_uri(f'wss://localhost:{port}/echo'))
                peer.sendall(protocol.connect().serialize())
                response=b''
                while not response.endswith(b'\r\n\r\n'):
                    chunk=peer.recv(4096)
                    if not chunk:raise ValueError('EOF during independent Upgrade')
                    response+=chunk
                protocol.receive_data(response)
                if protocol.state is not OPEN:raise ValueError('independent Upgrade failed')
                peer.sendall(b'\x88\x82\x01\x02\x03\x04\x02\xea') # masked Close(1000)
                response=b''
                while len(response)<4:
                    chunk=peer.recv(4-len(response))
                    if not chunk:raise ValueError('EOF during independent Close')
                    response+=chunk
                if response!=b'\x88\x02\x03\xe8':raise ValueError('independent Close echo mismatch')
                if graceful:
                    plain=peer.unwrap();plain.shutdown(socket.SHUT_WR);plain.close()
                else:peer.shutdown(socket.SHUT_WR) # intentionally omit close_notify
          closed(lines,'CLOSED 1 1 1' if graceful else 'CLOSED 13 8 0')
        rejected(port,wrong);closed(lines)
        rejected(port,context,'wrong.example');closed(lines)
        async def denied():
            try:
                async with connect(f'wss://127.0.0.1:{port}/denied',ssl=context,server_hostname='localhost',proxy=None,compression=None,open_timeout=3):pass
            except InvalidStatus as error:
                if error.response.status_code!=403:raise
                trace.write('policy-rejected',status=error.response.status_code)
            else:raise ValueError('WSS policy rejection bypassed')
        asyncio.run(denied())
    text=server('interop','server.pem',healthy)
    if len(re.findall(r'^CLOSED 1 1 1$',text,re.M))!=9:raise ValueError('independent WS/TLS close_notify missing')
    server('expired','expired.pem',lambda port,lines:rejected(port,context))
    verify_peer_diagnostics(out)
    result={'implementation':'websockets','client_api':'asyncio','version':websockets.__version__,'generations':8,'messages':16,'pings':8,'policy_rejections':1,'certificate_rejections':3,'graceful_closes':9,'truncated_closes':1,'status':'passed'}
    verify_interop(result);return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',choices=['release','asan','tsan'],required=True)
    parser.add_argument('--build-dir',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--openssl-root',type=Path);args=parser.parse_args()
    repo=Path(__file__).resolve().parents[2];build=args.build_dir.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    config='Release' if args.profile=='release' else 'Debug';env=os.environ.copy()
    if args.profile=='asan':env.update(ASAN_OPTIONS='halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if args.profile=='tsan':env['TSAN_OPTIONS']='halt_on_error=1'
    manifest={'schema':'gamenet.websocket_tls_contracts.v2','status':'failed','profile':args.profile,'workflow_sha':env.get('GITHUB_SHA'),'run_id':env.get('GITHUB_RUN_ID'),'run_attempt':env.get('GITHUB_RUN_ATTEMPT')}
    def execute(name,command,succeeds=True):
        print('Running '+name,flush=True);(out/(name+'-command.json')).write_text(json.dumps(command,indent=2)+'\n',encoding='utf-8')
        with (out/(name+'.log')).open('wb') as log:r=subprocess.run(command,cwd=repo,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
        text=(out/(name+'.log')).read_text(encoding='utf-8',errors='replace')
        if (r.returncode==0)!=succeeds:print(text[-12000:]);raise RuntimeError(f'{name}: unexpected exit {r.returncode}')
        return text
    def configure(directory,ws=True,tls=True,sanitized=True):
        command=['cmake','-S',str(repo),'-B',str(directory),'-DCMAKE_BUILD_TYPE='+config,'-DGAMENET_ENABLE_WEBSOCKET='+('ON' if ws else 'OFF'),'-DGAMENET_ENABLE_TLS='+('ON' if tls else 'OFF'),'-DGAMENET_BUILD_TESTING=ON','-DGAMENET_BUILD_BENCHMARKS=OFF','-DGAMENET_BUILD_FUZZING=OFF','-DGAMENET_ENABLE_EXPERIMENTAL=OFF']
        command+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        if sanitized and args.profile=='asan':command+=['-DGAMENET_ENABLE_ASAN_UBSAN=ON']
        if sanitized and args.profile=='tsan':command+=['-DGAMENET_ENABLE_TSAN=ON']
        if not tls:command+=['-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON']
        if tls and args.openssl_root:command+=['-DOPENSSL_ROOT_DIR='+str(args.openssl_root.resolve())]
        return command
    def consumer(name,prefix,component,succeeds=True):
        command=['cmake','-S',str(repo/'tests/cmake/websocket_tls_install_consumer'),'-B',str(build/name),'-DCMAKE_BUILD_TYPE='+config,'-DCMAKE_PREFIX_PATH='+str(prefix),'-DREQUEST_COMPONENT='+component]
        command+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        if component in {'core','websocket'} or not succeeds:command+=['-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON']
        elif args.openssl_root:command+=['-DOPENSSL_ROOT_DIR='+str(args.openssl_root.resolve())]
        text=execute(name+'-configure',command,succeeds)
        if not succeeds:
            if 'Required component websocket_tls is unavailable' not in text:raise ValueError('unavailable WSS rejected for wrong reason')
            return
        execute(name+'-build',['cmake','--build',str(build/name),'--config',config,'--parallel','2'])
        text=execute(name+'-test',['ctest','--test-dir',str(build/name),'-C',config,'--no-tests=error','--output-on-failure'])
        if '100% tests passed' not in text:raise ValueError('installed consumer did not execute')
    try:
        manifest['commit_sha']=execute('commit',['git','rev-parse','HEAD']).strip();manifest['working_tree']=execute('worktree',['git','status','--porcelain=v1'])
        if manifest['workflow_sha'] and (manifest['workflow_sha']!=manifest['commit_sha'] or manifest['working_tree']):raise ValueError('exact clean workflow checkout required')
        execute('configure',configure(build))
        for path in (build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
            match=re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"',path.read_text(encoding='utf-8'))
            if match:env['PATH']=str(Path(match[1]).parent)+os.pathsep+env.get('PATH','');break
        inventory=verify_inventory(json.loads(execute('inventory',['ctest','--test-dir',str(build),'-C',config,'--show-only=json-v1'])))
        installed=['gamenet_'+n.split('::')[1] for group in json.loads((repo/'api/public_api_manifest.json').read_text())['targets'].values() for n in group if n!='GameNet::udp']
        targets=['gnt_'+n.split('.')[0]+'_'+n.split('.')[1]+'_'+hashlib.sha1(n.encode()).hexdigest()[:10] for n in sorted(CONTRACTS)]
        command=['cmake','--build',str(build),'--config',config,'--parallel','2']
        integration=build/'tests'/('' if os.name!='nt' else config)/('gnt_integration_websocket_tls_'+hashlib.sha1(b'integration.websocket_tls.test_websocket_tls_tcp').hexdigest()[:10]+('.exe' if os.name=='nt' else ''))
        if args.profile!='release':command+=['--target',*installed,*targets,'gamenet_websocket_tls_echo_server']
        execute('build',command)
        command=['ctest','--test-dir',str(build),'-C',config,'--output-on-failure','--timeout','60','--no-tests=error','--output-junit',str(out/'ctest-junit.xml')]
        if args.profile!='release':command+=['-R',r'^(unit|contract|integration)\.(websocket|websocket_tls|tls|rpc_tls)\.']
        execute('ctest',command);expected=inventory if args.profile=='release' else CONTRACTS
        verify_results(ET.parse(out/'ctest-junit.xml').getroot(),expected);manifest['executed_tests']=len(expected)
        certs=build/'interop-certificates';execute('certificates',[str(integration),str(certs)])
        manifest['timeout_race_proof']=reproduce(certs,out/'timeout-race-proof.json')
        binary=build/'tests'/('' if os.name!='nt' else config)/('gamenet_websocket_tls_echo_server'+('.exe' if os.name=='nt' else ''))
        rounds=[]
        for round_id in range(3):
            location=out/('round-'+str(round_id));location.mkdir(exist_ok=True)
            rounds.append(interoperate(binary,certs,location,env))
        verify_interop_rounds(rounds);manifest['interop_rounds']=rounds
        manifest['interop']=rounds[0]
        prefix=build/'prefix';execute('install',['cmake','--install',str(build),'--config',config,'--prefix',str(prefix)])
        for component in ['core','websocket','websocket_tls']:consumer(component+'-consumer',prefix,component)
        if args.profile=='release':
            manifest['off_isolation']=[]
            for label,ws,tls in [('ws-only',True,False),('tls-only',False,True),('both-off',False,False)]:
                off=build/label;execute(label+'-configure',configure(off,ws,tls,False))
                available=[t for t in installed if t!='gamenet_websocket_tls' and (ws or t!='gamenet_websocket') and (tls or t not in {'gamenet_tls','gamenet_rpc_tls'})]
                execute(label+'-build',['cmake','--build',str(off),'--config',config,'--parallel','2','--target',*available])
                off_prefix=off/'prefix';execute(label+'-install',['cmake','--install',str(off),'--config',config,'--prefix',str(off_prefix)])
                if (off_prefix/'include/gamenet/websocket_tls').exists() or list(off_prefix.rglob('GameNetWebSocketTlsTargets.cmake')):raise ValueError('OFF leaked WSS header/export')
                consumer(label+'-core',off_prefix,'core')
                if ws:consumer(label+'-ws',off_prefix,'websocket')
                consumer(label+'-wss-rejection',off_prefix,'websocket_tls',False);manifest['off_isolation'].append(label)
        manifest['status']='passed'
    finally:
        manifest['files']={p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file() and p.name!='manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
