# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Exact KCP-C1 inventory, independent standard peer and install isolation."""
from __future__ import annotations
import argparse,hashlib,json,os,platform,re,subprocess,sys
from pathlib import Path
import xml.etree.ElementTree as ET
from kcp_interop import interoperate, verify_evidence, EXPECTED as INTEROP
from run_udp_contracts import CONTRACTS as UDP_CONTRACTS
CONTRACTS={'unit.kcp.test_kcp_engine','contract.kcp.test_kcp_transport','integration.kcp.test_kcp_reliable'}

def verify_inventory(document):
    tests=document.get('tests',[]);names=[t['name'] for t in tests]
    if len(names)!=141 or len(set(names))!=141:raise ValueError('KCP inventory must be exactly 141')
    selected=set()
    for t in tests:
        props={p['name']:p['value'] for p in t.get('properties',[])}
        if 'kcp' in props.get('LABELS',[]):
            if props.get('DISABLED',False):raise ValueError('disabled KCP contract')
            selected.add(t['name'])
    if selected!=CONTRACTS:raise ValueError('exact three KCP tests required')
    if not UDP_CONTRACTS<=set(names):raise ValueError('UDP prerequisite contracts missing')
    return set(names)

def verify_results(root,expected):
    tests=root.findall('.//testcase')
    if len(tests)!=len(expected) or {t.get('name') for t in tests}!=expected:raise ValueError('KCP execution mismatch')
    for t in tests:
        if t.get('status')!='run' or any(t.find(k) is not None for k in ('failure','error','skipped')):raise ValueError('unexecuted/failed KCP test')

def verify_interop(value):
    if value!=INTEROP:raise ValueError('independent standard KCP impairment evidence missing')

def verify_output(output):
    output=Path(output);d=json.loads((output/'manifest.json').read_text(encoding='utf-8'))
    if d.get('schema')!='gamenet.kcp_contracts.v1' or d.get('status')!='passed':raise ValueError('incomplete KCP evidence')
    if not re.fullmatch('[0-9a-f]{40}',d.get('commit_sha','')):raise ValueError('invalid KCP source identity')
    if d.get('workflow_sha') and (d['workflow_sha']!=d['commit_sha'] or d['working_tree']):raise ValueError('wrong/dirty KCP source')
    for key,env in [('workflow_sha','GITHUB_SHA'),('run_id','GITHUB_RUN_ID'),('run_attempt','GITHUB_RUN_ATTEMPT')]:
        if os.getenv(env) and d.get(key)!=os.getenv(env):raise ValueError('wrong KCP run identity')
    files={}
    for p in output.rglob('*'):
        if p.is_symlink():raise ValueError('KCP evidence symlink')
        if p.is_file() and p!=output/'manifest.json':files[p.relative_to(output).as_posix()]=hashlib.sha256(p.read_bytes()).hexdigest()
    if files!=d.get('files'):raise ValueError('KCP evidence file/hash mismatch')
    inventory=verify_inventory(json.loads((output/'inventory.log').read_text(encoding='utf-8')))
    expected=inventory if d['profile']=='release' else CONTRACTS|UDP_CONTRACTS
    verify_results(ET.parse(output/'ctest-junit.xml').getroot(),expected)
    if d.get('executed_tests')!=len(expected):raise ValueError('KCP count drift')
    verify_interop(d.get('interop'))
    verify_evidence(output/'interop')
    if len(d.get('binary_sha256',{}))!=2 or not all(re.fullmatch('[0-9a-f]{64}',s) for s in d['binary_sha256'].values()):raise ValueError('invalid KCP binary identities')
    if d.get('consumers')!=['kcp','udp','core']:raise ValueError('KCP package consumer missing')
    if d['profile']=='release' and d.get('off_isolation')!='passed':raise ValueError('KCP OFF evidence missing')
    return d

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--profile',choices=['release','asan','tsan'])
    p.add_argument('--build-dir',type=Path);p.add_argument('--output',type=Path);p.add_argument('--verify-output',type=Path);a=p.parse_args()
    if a.verify_output:verify_output(a.verify_output);print('KCP contracts readback passed');return
    if not(a.profile and a.build_dir and a.output):p.error('profile/build-dir/output required')
    repo=Path(__file__).resolve().parents[2];build=a.build_dir.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();config='Release' if a.profile=='release' else 'Debug'
    if a.profile=='asan':env.update(ASAN_OPTIONS='halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if a.profile=='tsan':env['TSAN_OPTIONS']='halt_on_error=1'
    d={'schema':'gamenet.kcp_contracts.v1','status':'failed','profile':a.profile,'platform':platform.platform(),
       'workflow_sha':env.get('GITHUB_SHA'),'run_id':env.get('GITHUB_RUN_ID'),'run_attempt':env.get('GITHUB_RUN_ATTEMPT')}
    def run(name,cmd,success=True):
        (out/(name+'-command.json')).write_text(json.dumps(cmd,indent=2)+'\n',encoding='utf-8');print('Running '+name,flush=True)
        with (out/(name+'.log')).open('wb') as log:r=subprocess.run(cmd,cwd=repo,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
        text=(out/(name+'.log')).read_text(encoding='utf-8',errors='replace')
        if (r.returncode==0)!=success:print(text[-12000:]);raise RuntimeError(name+': unexpected exit '+str(r.returncode))
        return text
    def configure(directory,kcp,udp=True):
        cmd=['cmake','-S',str(repo),'-B',str(directory),'-DCMAKE_BUILD_TYPE='+config,'-DGAMENET_ENABLE_KCP='+('ON' if kcp else 'OFF'),
            '-DGAMENET_ENABLE_UDP='+('ON' if udp else 'OFF'),'-DGAMENET_ENABLE_TLS=OFF','-DGAMENET_ENABLE_WEBSOCKET=OFF',
            '-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON','-DGAMENET_BUILD_TESTING=ON','-DGAMENET_BUILD_BENCHMARKS=OFF']
        cmd+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        if kcp and a.profile=='asan':cmd+=['-DGAMENET_ENABLE_ASAN_UBSAN=ON']
        if kcp and a.profile=='tsan':cmd+=['-DGAMENET_ENABLE_TSAN=ON']
        return cmd
    def consumer(name,prefix,mode,success=True):
        directory=build/name
        cmd=['cmake','-S',str(repo/'tests/cmake/kcp_install_consumer'),'-B',str(directory),'-DCMAKE_BUILD_TYPE='+config,
            '-DCMAKE_PREFIX_PATH='+str(prefix),'-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON','-DMODE='+mode]
        cmd+=['-A','x64'] if os.name=='nt' else ['-G','Ninja']
        text=run(name+'-configure',cmd,success)
        if not success:
            if 'Required component kcp is unavailable' not in text:raise ValueError('KCP rejection failed for wrong reason')
            return
        run(name+'-build',['cmake','--build',str(directory),'--config',config,'--parallel','2'])
        run(name+'-test',['ctest','--test-dir',str(directory),'-C',config,'--output-on-failure','--no-tests=error'])
    try:
        d['commit_sha']=run('commit',['git','rev-parse','HEAD']).strip();d['working_tree']=run('worktree',['git','status','--porcelain=v1'])
        if d['workflow_sha'] and (d['workflow_sha']!=d['commit_sha'] or d['working_tree']):raise ValueError('exact clean KCP source required')
        run('provenance',[sys.executable,'tools/check_spdx_headers.py'])
        run('configure',configure(build,True))
        for f in (build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
            m=re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)',f.read_text(encoding='utf-8'))
            if m:env['PATH']=str(Path(m[1]).parent)+os.pathsep+env.get('PATH','');break
        inventory=verify_inventory(json.loads(run('inventory',['ctest','--test-dir',str(build),'-C',config,'--show-only=json-v1'])))
        api=json.loads((repo/'api/public_api_manifest.json').read_text(encoding='utf-8'))
        installed=['gamenet_'+n.split('::')[1] for group in api['targets'].values() for n in group if n not in {'GameNet::tls','GameNet::rpc_tls','GameNet::websocket','GameNet::websocket_tls'}]
        selected=CONTRACTS|UDP_CONTRACTS
        targets=['gnt_'+n.split('.')[0]+'_'+n.split('.')[1]+'_'+hashlib.sha1(n.encode()).hexdigest()[:10] for n in sorted(selected)]
        cmd=['cmake','--build',str(build),'--config',config,'--parallel','2']
        if a.profile!='release':cmd+=['--target',*installed,*targets,'gamenet_kcp_interop_server','gamenet_kcp_reference']
        run('build',cmd)
        cmd=['ctest','--test-dir',str(build),'-C',config,'--output-on-failure','--timeout','60','--no-tests=error','--output-junit',str(out/'ctest-junit.xml')]
        if a.profile!='release':cmd+=['-L','^(kcp|udp)$']
        run('ctest',cmd);expected=inventory if a.profile=='release' else selected
        verify_results(ET.parse(out/'ctest-junit.xml').getroot(),expected);d['executed_tests']=len(expected)
        directory=build/'tests'/('' if os.name!='nt' else config);suffix='.exe' if os.name=='nt' else ''
        server=directory/('gamenet_kcp_interop_server'+suffix);reference=directory/('gamenet_kcp_reference'+suffix)
        d['binary_sha256']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in [server,reference]}
        d['interop']=interoperate(server,reference,out/'interop',env);verify_interop(d['interop'])
        prefix=build/'prefix';run('install',['cmake','--install',str(build),'--config',config,'--prefix',str(prefix)])
        if (prefix/'share/doc/GameNetCore/kcp/LICENSE').read_bytes()!=(repo/'third_party/kcp/LICENSE').read_bytes():raise ValueError('MIT license missing')
        for mode in ['kcp','udp','core']:consumer(mode+'-consumer',prefix,mode)
        d['consumers']=['kcp','udp','core']
        if a.profile=='release':
            text=run('kcp-without-udp',configure(build/'invalid',True,False),False)
            if 'requires explicit GAMENET_ENABLE_UDP=ON' not in text:raise ValueError('invalid dependency rejected for wrong reason')
            off=build/'off';run('off-configure',configure(off,False))
            run('off-build',['cmake','--build',str(off),'--config',config,'--parallel','2','--target',*[t for t in installed if t!='gamenet_kcp']])
            off_prefix=off/'prefix';run('off-install',['cmake','--install',str(off),'--config',config,'--prefix',str(off_prefix)])
            if (off_prefix/'include/gamenet/kcp').exists() or list(off_prefix.rglob('GameNetKcpTargets.cmake')) or list(off_prefix.rglob('*gamenet_kcp*')):raise ValueError('KCP leaked into OFF package')
            consumer('off-core',off_prefix,'core');consumer('off-udp',off_prefix,'udp');consumer('off-kcp',off_prefix,'kcp',False);d['off_isolation']='passed'
        d['status']='passed'
    finally:
        d['files']={f.relative_to(out).as_posix():hashlib.sha256(f.read_bytes()).hexdigest() for f in out.rglob('*') if f.is_file() and f!=out/'manifest.json'}
        (out/'manifest.json').write_text(json.dumps(d,indent=2)+'\n',encoding='utf-8')
    verify_output(out);print('KCP exact contracts, standard interoperability and package boundaries passed')
if __name__=='__main__':main()
