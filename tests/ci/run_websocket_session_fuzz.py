# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Bounded owner-session action fuzz with exact source/run evidence."""
from __future__ import annotations
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
from run_websocket_contracts import verify_fuzz

def corpus():
    def actions(*items):return bytes(x for item in items for x in (*item,*(0 for _ in range(4-len(item)))))
    result={
        'admission':actions((48,),(8,1),(7,1,16),(5,),(9,),(7,0,8),(10,1),(11,1)),
        'fragments':actions((48,),(4,),(4,),(5,),(6,),(9,),(10,1),(11,1)),
        'reentry':actions((15,5),(2,8),(15,6),(3,2),(14,),(15,4),(1,7),(2,2),(14,),(1,)),
        'terminal-send':actions((8,2),(7,1,2),(11,),(12,),(14,),(1,),(8,3),(5,)),
        'continuation':actions((48,),(17,),(9,),(17,1)),
        'replacement':actions((48,),(17,),(14,),(1,),(18,),(20,),(14,),(23,)),
        'control-limit':actions((8,1),(22,),(14,),(21,1),(1,),(13,)),
        'input-limit':actions((16,128),(11,),(14,),(23,),(3,36)),
    }
    for seed in range(1,33):
        state=seed;data=bytearray()
        for _ in range(512):state=(state*1664525+1013904223)&0xffffffff;data.append(state>>24)
        result['fixed-'+str(seed)]=bytes(data)
    return result

def verify_output(output):
    output=Path(output);d=json.loads((output/'manifest.json').read_text(encoding='utf-8'))
    if d.get('schema')!='gamenet.websocket_session_fuzz.v1' or d.get('status')!='passed':raise ValueError('incomplete session fuzz manifest')
    if (d.get('executed_units'),d.get('seed'),d.get('max_actions'),d.get('max_input_bytes'))!=(20000,424242,128,512):raise ValueError('session fuzz bounds/count/seed drift')
    if not re.fullmatch('[0-9a-f]{40}',d.get('commit_sha','')) or not re.fullmatch('[0-9a-f]{64}',d.get('binary_sha256','')):raise ValueError('invalid session source/binary identity')
    if d.get('workflow_sha') and (d['workflow_sha']!=d['commit_sha'] or d['working_tree']):raise ValueError('dirty or wrong session workflow source')
    for key,env in [('workflow_sha','GITHUB_SHA'),('run_id','GITHUB_RUN_ID'),('run_attempt','GITHUB_RUN_ATTEMPT')]:
        if os.getenv(env) and d.get(key)!=os.getenv(env):raise ValueError('wrong session run identity')
    files={}
    for p in output.rglob('*'):
        if p.is_symlink():raise ValueError('symlink in session evidence')
        if p.is_file() and p!=output/'manifest.json':files[p.relative_to(output).as_posix()]=hashlib.sha256(p.read_bytes()).hexdigest()
    if files!=d.get('files'):raise ValueError('session evidence file/hash mismatch')
    verify_fuzz((output/'fuzzer.log').read_text(encoding='utf-8'))
    command=json.loads((output/'command.json').read_text(encoding='utf-8'))
    for flag in ['-seed=424242','-runs=20000','-max_len=512','-timeout=5','-rss_limit_mb=512']:
        if command.count(flag)!=1:raise ValueError('session command drift')
    for name,seed in corpus().items():
        if not (output/'corpus'/name).is_file() or (output/'corpus'/name).read_bytes()!=seed:raise ValueError('missing/changed session seed')
    # Artifact ZIPs omit empty directories.
    if (output/'crashes').exists() and (not (output/'crashes').is_dir() or list((output/'crashes').iterdir())):raise ValueError('session crash artifact exists')
    return d

def run(build,output):
    repo=Path(__file__).resolve().parents[2];output=output.resolve();output.mkdir(parents=True,exist_ok=True)
    def git(*args):return subprocess.check_output(['git',*args],cwd=repo,text=True,encoding='utf-8').strip()
    manifest={'schema':'gamenet.websocket_session_fuzz.v1','status':'failed','commit_sha':git('rev-parse','HEAD'),
        'working_tree':git('status','--porcelain=v1'),'workflow_sha':os.getenv('GITHUB_SHA'),'run_id':os.getenv('GITHUB_RUN_ID'),'run_attempt':os.getenv('GITHUB_RUN_ATTEMPT')}
    try:
        if manifest['workflow_sha'] and (manifest['commit_sha']!=manifest['workflow_sha'] or manifest['working_tree']):raise ValueError('exact clean session fuzz checkout required')
        seeds=output/'corpus';crashes=output/'crashes'
        if seeds.exists() or crashes.exists():raise ValueError('fresh session fuzz evidence required')
        seeds.mkdir();crashes.mkdir()
        for name,value in corpus().items():(seeds/name).write_bytes(value)
        binary=(build/'tests/fuzz/gamenet_fuzz_websocket_session').resolve()
        command=[str(binary),str(seeds),'-seed=424242','-runs=20000','-max_len=512','-timeout=5','-rss_limit_mb=512','-print_final_stats=1','-artifact_prefix='+str(crashes)+'/']
        (output/'command.json').write_text(json.dumps(command,indent=2)+'\n',encoding='utf-8')
        with (output/'fuzzer.log').open('wb') as log:
            result=subprocess.run(command,cwd=repo,stdout=log,stderr=subprocess.STDOUT,
                env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'},timeout=600)
        if result.returncode:raise RuntimeError('session fuzzer failed; retained '+str(output))
        verify_fuzz((output/'fuzzer.log').read_text(encoding='utf-8'))
        if list(crashes.iterdir()):raise ValueError('session fuzz crash artifact exists')
        manifest.update(status='passed',executed_units=20000,seed=424242,max_actions=128,max_input_bytes=512,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    finally:
        manifest['files']={p.relative_to(output).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in output.rglob('*') if p.is_file() and p.name!='manifest.json'}
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print('WS session: verified 20,000 units, seed 424242, at most 128 actions',flush=True)
    verify_output(output)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build-dir',type=Path);parser.add_argument('--output',type=Path);parser.add_argument('--verify-output',type=Path)
    args=parser.parse_args()
    if args.verify_output:verify_output(args.verify_output);print('WS session independent evidence readback passed')
    elif args.build_dir and args.output:run(args.build_dir,args.output)
    else:parser.error('build-dir/output or verify-output required')
