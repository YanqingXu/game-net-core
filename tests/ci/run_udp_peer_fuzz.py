# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""20,000 real-owner bounded UDP peer action sequences, with evidence readback."""
from __future__ import annotations
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path

def corpus():
    result={'rotate':bytes([0,2,2,4,0,2,8,7]),'pressure':bytes([0]+[2]*12+[1,3,4,0,5]),
            'reentry':bytes([0,22,38,5,54,5,70,2,5]),'idle':bytes([0,1,7,7,0,4,0])}
    for seed in range(1,33):
        state=seed;data=bytearray()
        for _ in range(96):state=(state*1664525+1013904223)&0xffffffff;data.append(state>>24)
        result['fixed-'+str(seed)]=bytes(data)
    return result

def verify_log(text):
    if re.findall(r'stat::number_of_executed_units:\s+(\d+)',text)!=['20000'] or 'Seed: 424242\n' not in text:raise ValueError('UDP fuzz count/seed mismatch')
    if re.search(r'ERROR:|runtime error:|DEADLYSIGNAL|SUMMARY:.*Sanitizer',text):raise ValueError('UDP fuzz diagnostic')

def verify_output(output):
    output=Path(output);d=json.loads((output/'manifest.json').read_text(encoding='utf-8'))
    if d.get('schema')!='gamenet.udp_peer_fuzz.v1' or d.get('status')!='passed':raise ValueError('incomplete UDP peer fuzz evidence')
    if (d.get('executed_units'),d.get('seed'),d.get('max_actions'),d.get('max_input_bytes'))!=(20000,424242,96,96):raise ValueError('UDP fuzz bounds mismatch')
    if not re.fullmatch('[0-9a-f]{40}',d.get('commit_sha','')) or not re.fullmatch('[0-9a-f]{64}',d.get('binary_sha256','')):raise ValueError('invalid fuzz identities')
    if d.get('workflow_sha') and (d['workflow_sha']!=d['commit_sha'] or d['working_tree']):raise ValueError('wrong/dirty fuzz source')
    for key,env in [('workflow_sha','GITHUB_SHA'),('run_id','GITHUB_RUN_ID'),('run_attempt','GITHUB_RUN_ATTEMPT')]:
        if os.getenv(env) and d.get(key)!=os.getenv(env):raise ValueError('wrong fuzz run identity')
    files={}
    for p in output.rglob('*'):
        if p.is_symlink():raise ValueError('fuzz symlink')
        if p.is_file() and p!=output/'manifest.json':files[p.relative_to(output).as_posix()]=hashlib.sha256(p.read_bytes()).hexdigest()
    if files!=d.get('files'):raise ValueError('fuzz file/hash mismatch')
    verify_log((output/'fuzzer.log').read_text(encoding='utf-8'))
    command=json.loads((output/'command.json').read_text(encoding='utf-8'))
    for flag in ['-seed=424242','-runs=20000','-max_len=96','-timeout=5','-rss_limit_mb=512']:
        if command.count(flag)!=1:raise ValueError('fuzz command drift')
    for name,data in corpus().items():
        if not (output/'corpus'/name).is_file() or (output/'corpus'/name).read_bytes()!=data:raise ValueError('fuzz corpus drift')
    if (output/'crashes').exists() and list((output/'crashes').iterdir()):raise ValueError('fuzz crash retained')
    return d

def run(build,output):
    repo=Path(__file__).resolve().parents[2];output=output.resolve();output.mkdir(parents=True,exist_ok=True)
    def git(*args):return subprocess.check_output(['git',*args],cwd=repo,text=True,encoding='utf-8').strip()
    d={'schema':'gamenet.udp_peer_fuzz.v1','status':'failed','commit_sha':git('rev-parse','HEAD'),
       'working_tree':git('status','--porcelain=v1'),'workflow_sha':os.getenv('GITHUB_SHA'),'run_id':os.getenv('GITHUB_RUN_ID'),'run_attempt':os.getenv('GITHUB_RUN_ATTEMPT')}
    try:
        if d['workflow_sha'] and (d['workflow_sha']!=d['commit_sha'] or d['working_tree']):raise ValueError('exact clean fuzz source required')
        seeds=output/'corpus';crashes=output/'crashes';seeds.mkdir();crashes.mkdir()
        for name,data in corpus().items():(seeds/name).write_bytes(data)
        binary=(build/'tests/fuzz/gamenet_fuzz_udp_peer').resolve()
        command=[str(binary),str(seeds),'-seed=424242','-runs=20000','-max_len=96','-timeout=5','-rss_limit_mb=512','-print_final_stats=1','-artifact_prefix='+str(crashes)+'/']
        (output/'command.json').write_text(json.dumps(command,indent=2)+'\n',encoding='utf-8')
        with (output/'fuzzer.log').open('wb') as log:
            r=subprocess.run(command,cwd=repo,stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'},timeout=600)
        if r.returncode:raise RuntimeError('UDP peer fuzzer failed; raw evidence retained')
        verify_log((output/'fuzzer.log').read_text(encoding='utf-8'))
        if list(crashes.iterdir()):raise ValueError('UDP peer fuzz crash')
        d.update(status='passed',executed_units=20000,seed=424242,max_actions=96,max_input_bytes=96,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    finally:
        d['files']={p.relative_to(output).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in output.rglob('*') if p.is_file() and p!=output/'manifest.json'}
        (output/'manifest.json').write_text(json.dumps(d,indent=2)+'\n',encoding='utf-8')
    verify_output(output);print('UDP peer fuzz verified: 20,000 units, seed 424242, 96 actions maximum',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path);p.add_argument('--output',type=Path);p.add_argument('--verify-output',type=Path);a=p.parse_args()
    if a.verify_output:verify_output(a.verify_output);print('UDP peer fuzz readback passed')
    elif a.build_dir and a.output:run(a.build_dir,a.output)
    else:p.error('build-dir/output or verify-output required')
