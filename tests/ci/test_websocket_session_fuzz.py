# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import copy,hashlib,json,os,tempfile
from run_websocket_contracts import verify_fuzz
from run_websocket_session_fuzz import corpus,verify_output

def main():
    seeds=corpus();assert len(seeds)==40 and all(0<len(value)<=512 and len(value)%4==0 for value in seeds.values())
    valid='INFO: Seed: 424242\nstat::number_of_executed_units: 20000\n';verify_fuzz(valid)
    for bad in ['',valid.replace('20000','19999'),valid.replace('424242','42'),valid+valid,valid+'ERROR: AddressSanitizer']:
        try:verify_fuzz(bad)
        except ValueError:pass
        else:raise AssertionError('invalid session fuzz evidence accepted')
    with tempfile.TemporaryDirectory(prefix='ws-state-evidence-') as directory:
        p=Path(directory);(p/'corpus').mkdir();(p/'crashes').mkdir()
        for name,value in seeds.items():(p/'corpus'/name).write_bytes(value)
        (p/'fuzzer.log').write_text(valid)
        (p/'command.json').write_text(json.dumps(['fuzzer','-seed=424242','-runs=20000','-max_len=512','-timeout=5','-rss_limit_mb=512']))
        good={'schema':'gamenet.websocket_session_fuzz.v1','status':'passed','commit_sha':os.getenv('GITHUB_SHA','1'*40),'working_tree':'',
            'workflow_sha':os.getenv('GITHUB_SHA'),'run_id':os.getenv('GITHUB_RUN_ID'),'run_attempt':os.getenv('GITHUB_RUN_ATTEMPT'),
            'executed_units':20000,'seed':424242,'max_actions':128,'max_input_bytes':512,'binary_sha256':'2'*64,
            'files':{f.relative_to(p).as_posix():hashlib.sha256(f.read_bytes()).hexdigest() for f in p.rglob('*') if f.is_file()}}
        (p/'manifest.json').write_text(json.dumps(good));verify_output(p)
        for field,value in [('executed_units',19999),('seed',1),('max_actions',129),('max_input_bytes',513),('status','failed'),('files',{})]:
            bad=copy.deepcopy(good);bad[field]=value;(p/'manifest.json').write_text(json.dumps(bad))
            try:verify_output(p)
            except ValueError:pass
            else:raise AssertionError('session manifest drift accepted: '+field)
        (p/'manifest.json').write_text(json.dumps(good));(p/'fuzzer.log').write_text('changed')
        try:verify_output(p)
        except ValueError:pass
        else:raise AssertionError('changed session artifact accepted')
    root=Path(__file__).resolve().parents[2]
    cmake=(root/'tests/fuzz/CMakeLists.txt').read_text()
    for token in ['gamenet_fuzz_websocket_session','websocket/fuzz_websocket_session.cpp','GameNet::websocket','-fsanitize=fuzzer,address,undefined']:
        assert token in cmake
    driver=(root/'tests/ci/run_websocket_contracts.py').read_text()
    for token in ['gamenet_fuzz_websocket_session','run_websocket_session_fuzz.py',"manifest['session_fuzz']"]:assert token in driver
    workflow=(root/'.github/workflows/websocket.yml').read_text()
    assert 'test_websocket_session_fuzz.py' in workflow and 'run_websocket_session_fuzz.py --verify-output' in workflow
    assert 'continue-on-error' not in workflow
    print('WS state fuzz seeds, exact run evidence and required instrumentation passed')
if __name__=='__main__':main()
