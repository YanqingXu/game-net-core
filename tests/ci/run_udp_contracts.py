# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Exact optional UDP inventory, native contracts, independent peers and packages."""
from __future__ import annotations
import argparse,hashlib,json,os,platform,subprocess,sys
from pathlib import Path
import xml.etree.ElementTree as ET
from udp_interop import interoperate
from udp_peer_interop import interoperate as peer_interoperate, EXPECTED as PEER_INTEROP

CONTRACTS={'unit.udp.test_udp_options','contract.udp.test_udp_socket','integration.udp.test_udp_stress','contract.udp.test_udp_peer','integration.udp.test_udp_peer_stress'}
INTEROP={'implementation':'python.socket','families':[4,6],'generations':8,'datagrams':48,'empty_datagrams':8,'truncated_datagrams':16,'status':'passed'}
def verify_inventory(document):
    tests=document.get('tests',[]);names=[t['name'] for t in tests]
    if len(names)!=138 or len(set(names))!=138:raise ValueError('UDP inventory must be exactly 138')
    selected=set()
    for t in tests:
        props={p['name']:p['value'] for p in t.get('properties',[])}
        if 'udp' in props.get('LABELS',[]):
            if props.get('DISABLED',False):raise ValueError('disabled UDP contract')
            selected.add(t['name'])
    if selected!=CONTRACTS:raise ValueError('exact five UDP tests required')
    return set(names)
def verify_results(root,expected):
    tests=root.findall('.//testcase')
    if len(tests)!=len(expected) or {c.get('name') for c in tests}!=expected:raise ValueError('UDP execution inventory mismatch')
    for c in tests:
        if c.get('status')!='run' or any(c.find(k) is not None for k in ['failure','error','skipped']):raise ValueError('UDP case failed/skipped/not executed')
def verify_interop(value):
    if value!=INTEROP:raise ValueError('independent dual-family UDP evidence missing')

def verify_peer_interop(value):
    if value!=PEER_INTEROP:raise ValueError('independent explicit-peer/generation UDP evidence missing')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--profile',choices=['release','asan','tsan'],required=True)
    parser.add_argument('--build-dir',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    repo=Path(__file__).resolve().parents[2];build=args.build_dir.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();config='Release' if args.profile=='release' else 'Debug'
    if args.profile=='asan':env.update(ASAN_OPTIONS='halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if args.profile=='tsan':env['TSAN_OPTIONS']='halt_on_error=1'
    manifest={'schema':'gamenet.udp_contracts.v2','status':'failed','profile':args.profile,'platform':platform.platform(),'python':sys.version,
        'workflow_sha':env.get('GITHUB_SHA'),'run_id':env.get('GITHUB_RUN_ID'),'run_attempt':env.get('GITHUB_RUN_ATTEMPT')}
    def execute(name,command,succeeds=True):
        (out/(name+'-command.json')).write_text(json.dumps(command,indent=2)+'\n',encoding='utf-8');print('Running '+name,flush=True)
        with (out/(name+'.log')).open('wb') as log:r=subprocess.run(command,cwd=repo,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
        text=(out/(name+'.log')).read_text(encoding='utf-8',errors='replace')
        if (r.returncode==0)!=succeeds:print(text[-12000:]);raise RuntimeError(name+': unexpected exit '+str(r.returncode))
        return text
    def configure(directory,enabled):
        command=['cmake','-S',str(repo),'-B',str(directory),'-DCMAKE_BUILD_TYPE='+config,'-DGAMENET_ENABLE_UDP='+('ON' if enabled else 'OFF'),
            '-DGAMENET_ENABLE_TLS=OFF','-DGAMENET_ENABLE_WEBSOCKET=OFF','-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON','-DGAMENET_BUILD_TESTING=ON','-DGAMENET_BUILD_BENCHMARKS=OFF']
        command+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        if enabled and args.profile=='asan':command+=['-DGAMENET_ENABLE_ASAN_UBSAN=ON']
        if enabled and args.profile=='tsan':command+=['-DGAMENET_ENABLE_TSAN=ON']
        return command
    def consumer(name,prefix,udp,succeeds=True):
        directory=build/name
        command=['cmake','-S',str(repo/'tests/cmake/udp_install_consumer'),'-B',str(directory),'-DCMAKE_BUILD_TYPE='+config,
            '-DCMAKE_PREFIX_PATH='+str(prefix),'-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON','-DREQUEST_UDP='+('ON' if udp else 'OFF')]
        command+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        text=execute(name+'-configure',command,succeeds)
        if not succeeds:
            if 'Required component udp is unavailable' not in text:raise ValueError('UDP OFF rejection failed for wrong reason')
            return
        execute(name+'-build',['cmake','--build',str(directory),'--config',config,'--parallel','2'])
        text=execute(name+'-test',['ctest','--test-dir',str(directory),'-C',config,'--output-on-failure','--no-tests=error'])
        if '100% tests passed' not in text:raise ValueError('UDP consumer did not execute')
    try:
        manifest['commit_sha']=execute('commit',['git','rev-parse','HEAD']).strip()
        manifest['working_tree']=execute('worktree',['git','status','--porcelain=v1'])
        if manifest['workflow_sha'] and (manifest['workflow_sha']!=manifest['commit_sha'] or manifest['working_tree']):raise ValueError('exact clean UDP source required')
        execute('configure',configure(build,True))
        for p in (build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
            import re
            m=re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)',p.read_text(encoding='utf-8'))
            if m:env['PATH']=str(Path(m[1]).parent)+os.pathsep+env.get('PATH','');break
        inventory=verify_inventory(json.loads(execute('inventory',['ctest','--test-dir',str(build),'-C',config,'--show-only=json-v1'])))
        api=json.loads((repo/'api/public_api_manifest.json').read_text(encoding='utf-8'))
        installed=['gamenet_'+name.split('::')[1] for group in api['targets'].values() for name in group if name not in {'GameNet::tls','GameNet::rpc_tls','GameNet::websocket','GameNet::websocket_tls'}]
        targets=['gnt_'+n.split('.')[0]+'_udp_'+hashlib.sha1(n.encode()).hexdigest()[:10] for n in sorted(CONTRACTS)]
        command=['cmake','--build',str(build),'--config',config,'--parallel','2']
        if args.profile!='release':command+=['--target',*installed,*targets,'gamenet_udp_echo_server','gamenet_udp_peer_echo_server']
        execute('build',command)
        command=['ctest','--test-dir',str(build),'-C',config,'--output-on-failure','--timeout','60','--no-tests=error','--output-junit',str(out/'ctest-junit.xml')]
        if args.profile!='release':command+=['-L','^udp$']
        execute('ctest',command);expected=inventory if args.profile=='release' else CONTRACTS
        verify_results(ET.parse(out/'ctest-junit.xml').getroot(),expected);manifest['executed_tests']=len(expected)
        binary=build/'examples'/('' if os.name!='nt' else config)/('gamenet_udp_echo_server'+('.exe' if os.name=='nt' else ''))
        manifest['binary_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
        manifest['interop']=interoperate(binary,out/'interop',env);verify_interop(manifest['interop'])
        peer_binary=binary.with_name('gamenet_udp_peer_echo_server'+('.exe' if os.name=='nt' else ''))
        manifest['peer_binary_sha256']=hashlib.sha256(peer_binary.read_bytes()).hexdigest()
        manifest['peer_interop']=peer_interoperate(peer_binary,out/'peer-interop',env);verify_peer_interop(manifest['peer_interop'])
        prefix=build/'prefix';execute('install',['cmake','--install',str(build),'--config',config,'--prefix',str(prefix)])
        consumer('udp-consumer',prefix,True);consumer('core-consumer',prefix,False)
        if args.profile=='release':
            off=build/'off';execute('off-configure',configure(off,False))
            execute('off-build',['cmake','--build',str(off),'--config',config,'--parallel','2','--target',*[t for t in installed if t!='gamenet_udp']])
            off_prefix=off/'prefix';execute('off-install',['cmake','--install',str(off),'--config',config,'--prefix',str(off_prefix)])
            if (off_prefix/'include/gamenet/udp').exists() or list(off_prefix.rglob('GameNetUdpTargets.cmake')):raise ValueError('UDP leaked into OFF package')
            consumer('off-core-consumer',off_prefix,False);consumer('off-udp-rejection',off_prefix,True,False);manifest['off_isolation']='passed'
        manifest['status']='passed'
    finally:
        manifest['files']={p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file() and p!=out/'manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
