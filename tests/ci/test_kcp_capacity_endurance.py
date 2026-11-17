# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""C4 semantic and false-green evidence contracts, independent of the native tool."""
import copy
import importlib.util
import unittest
import json
import os
import subprocess
import sys
import tempfile
from unittest import mock
from pathlib import Path

path=Path(__file__).resolve().parents[2]/'tools/run_kcp_capacity_endurance.py'
spec=importlib.util.spec_from_file_location('kcp_c4',path)
c4=importlib.util.module_from_spec(spec);spec.loader.exec_module(c4)

def events(n=8,family=0,duration=1800):
    ready={'type':'ready','schema':c4.CHILD_SCHEMA,'pid':123,'sessions':n,'family':family,
           'duration_ms':duration*1000,'conv_base':1000,'backend':'epoll','rss_bytes':1000000,
           'peak_rss_bytes':1000000,'hub_protocol_limit':n*1048576,'peer_protocol_limit':1048576,
           'pending_limit':2*n,'pending_byte_limit':4096*n,'max_proxy_datagrams':4}
    result=[ready]
    for cycle in [1,2]:
        result.append(dict(type='heartbeat',pid=123,sessions=n,cycle=cycle,
            family=(4 if cycle==1 else 6) if not family else family,
            elapsed_ms=duration*1000//2*cycle,**c4.expected_counts(n),wire_drops=n,ack_drops=n,
            arena_bytes=450000,peak_protocol_bytes=2*n*450000,peak_pending_messages=2*n,
            peak_pending_bytes=2048*n,proxy_peak_datagrams=2,residual=0,
            rss_bytes=1000000,peak_rss_bytes=2000000))
    result.append(dict(type='final',status='passed',pid=123,cycles=2,elapsed_ms=duration*1000,
                       residual=0,rss_bytes=1000000,peak_rss_bytes=2000000))
    return result

class Evidence(unittest.TestCase):
    def rejected(self,value,**kwargs):
        with self.assertRaises(ValueError):c4.validate_events(value,8,0,1800,123,**kwargs)
    def test_valid_dual(self):c4.validate_events(events(),8,0,1800,123)
    def test_valid_smoke(self):c4.validate_events(events(1,4,2),1,4,2,123)
    def test_short_soak(self):
        data=events();data[-1]['elapsed_ms']-=1;self.rejected(data)
    def test_process_replacement(self):
        data=events();data[1]['pid']=124;self.rejected(data)
    def test_missing_family(self):
        data=events();data[2]['family']=4;self.rejected(data)
    def test_missing_phase(self):
        data=events();data[1]['ack_drops']=0;self.rejected(data)
    def test_duplicate_receipt(self):
        data=events();data[1]['acknowledged']+=1;self.rejected(data)
    def test_missing_cancellation(self):
        data=events();data[1]['cancelled']=0;self.rejected(data)
    def test_fake_zero_memory(self):
        data=events();data[1]['arena_bytes']=0;self.rejected(data)
    def test_nonzero_residue(self):
        data=events();data[-1]['residual']=1;self.rejected(data)
    def test_budget_overflow(self):
        data=events();data[1]['peak_pending_bytes']=999999999;self.rejected(data)
    def test_nonmonotonic_heartbeat(self):
        data=events();data[2]['cycle']=1;self.rejected(data)
    def test_nonmonotonic_time(self):
        data=events();data[2]['elapsed_ms']=0;self.rejected(data)
    def test_truncated_stream(self):self.rejected(events()[:-1])
    def test_restarted_stream(self):self.rejected(events()+events())
    def test_reconfigured_limit(self):
        data=events();data[0]['pending_limit']+=1;self.rejected(data)
    def test_false_healthy_progress(self):
        data=events();data[1]['healthy_progress']=0;self.rejected(data)
    def test_proxy_unbounded(self):
        data=events();data[1]['proxy_peak_datagrams']=5;self.rejected(data)
    def test_missing_rss(self):
        data=events();data[1]['rss_bytes']=0;self.rejected(data)
    def test_release_rss_limit(self):
        data=events();data[1]['peak_rss_bytes']=600*1048576;self.rejected(data)
    def test_sanitizer_rss_is_diagnostic(self):
        data=events();data[1]['peak_rss_bytes']=600*1048576
        data[2]['peak_rss_bytes']=600*1048576;data[-1]['peak_rss_bytes']=600*1048576
        c4.validate_events(data,8,0,1800,123,sanitized=True)


class PortableEvidence(unittest.TestCase):
    def setUp(self):
        # Synthetic local evidence must not inherit the host workflow identity.
        # Keep PATH/runtime settings for the real malformed-child process test.
        self.environment=mock.patch.dict(os.environ,dict(GITHUB_SHA='',GITHUB_RUN_ID='',GITHUB_RUN_ATTEMPT=''))
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.temp=tempfile.TemporaryDirectory(prefix='kcp-c4-evidence-')
        self.addCleanup(self.temp.cleanup)
        self.out=Path(self.temp.name)
        self.d=dict(schema=c4.SCHEMA,status='passed',evidence_class='development',promotion=False,
                    profile='release',mode='smoke',platform='Linux',commit_sha='a'*40,
                    workflow_sha=None,run_id=None,run_attempt=None,working_tree='local',samples=[])
        source={}
        for name in c4.INPUTS:
            dest=self.out/'inputs'/name;dest.parent.mkdir(parents=True,exist_ok=True)
            dest.write_bytes((c4.REPO/name).read_bytes());source[name]=c4.digest(dest)
        self.d['source_inputs']=source
        (self.out/'source.json').write_text(json.dumps(source))
        (self.out/'native-tool.bin').write_bytes(b'fixture-not-executed')
        self.d['binary_sha256']=c4.digest(self.out/'native-tool.bin')
        (self.out/'commit.log').write_text('a'*40+'\n')
        (self.out/'worktree.log').write_text('local')
        (self.out/'CMakeCache.txt').write_text('\n'.join([
            'GAMENET_ENABLE_UDP:BOOL=ON','GAMENET_ENABLE_KCP:BOOL=ON',
            'GAMENET_BUILD_KCP_VALIDATION:BOOL=ON','GAMENET_ENABLE_ASAN_UBSAN:BOOL=OFF',
            'GAMENET_ENABLE_TSAN:BOOL=OFF']))
        (self.out/'compilation.json').write_text('[]')
        (self.out/'compiler.cmake').write_text('set(CMAKE_CXX_COMPILER_ID "GNU")\nset(CMAKE_CXX_COMPILER_VERSION "13.3.0")\n')
        self.d.update(compiler_id='GNU',compiler_version='13.3.0')
        for n in [1,8,32]:
            for family in [4,6]:
                stem=f'{n}-{family}'
                rows=events(n,family,2)
                sample=dict(sessions=n,family=family,duration_seconds=2,pid=123,returncode=0,
                    deadline_expired=False,events=stem+'.jsonl',stderr=stem+'.stderr',rss=stem+'.rss',
                    command=stem+'.command',wall_seconds=2.1,
                    started_utc='2026-11-17T00:00:00+00:00',finished_utc='2026-11-17T00:00:02.100+00:00',
                    proof=c4.validate_events(rows,n,family,2,123))
                (self.out/sample['events']).write_text(''.join(json.dumps(r)+'\n' for r in rows))
                (self.out/sample['stderr']).write_text('')
                (self.out/sample['rss']).write_text(''.join(json.dumps(dict(pid=123,elapsed_seconds=t,rss_bytes=1000000,peak_rss_bytes=2000000))+'\n' for t in [0.1,2.0]))
                (self.out/sample['command']).write_text(json.dumps(['tool','--sessions',str(n),'--family',str(family),'--duration-seconds','2','--conv-base','1000']))
                self.d['samples'].append(sample)
        self.seal()
    def seal(self):
        self.d['files']=c4.hashes(self.out)
        (self.out/'manifest.json').write_text(json.dumps(self.d))
    def reject(self,reseal=True):
        if reseal:self.seal()
        with self.assertRaises((ValueError,FileNotFoundError)):c4.verify_output(self.out)
    def test_valid_portable_readback(self):c4.verify_output(self.out)
    def ci_fixture(self):
        self.d.update(workflow_sha='a'*40,run_id='101',run_attempt='2',working_tree='')
        (self.out/'worktree.log').write_text('')
        self.seal()
        return dict(GITHUB_SHA='a'*40,GITHUB_RUN_ID='101',GITHUB_RUN_ATTEMPT='2')
    def test_ci_identity_matches(self):
        with mock.patch.dict(os.environ,self.ci_fixture()):c4.verify_output(self.out)
    def test_ci_sha_mismatch(self):
        env=self.ci_fixture();env['GITHUB_SHA']='b'*40
        with mock.patch.dict(os.environ,env):
            with self.assertRaisesRegex(ValueError,'wrong CI identity workflow_sha'):c4.verify_output(self.out)
    def test_ci_run_id_mismatch(self):
        env=self.ci_fixture();env['GITHUB_RUN_ID']='102'
        with mock.patch.dict(os.environ,env):
            with self.assertRaisesRegex(ValueError,'wrong CI identity run_id'):c4.verify_output(self.out)
    def test_ci_attempt_mismatch(self):
        env=self.ci_fixture();env['GITHUB_RUN_ATTEMPT']='3'
        with mock.patch.dict(os.environ,env):
            with self.assertRaisesRegex(ValueError,'wrong CI identity run_attempt'):c4.verify_output(self.out)
    def test_ci_identity_missing_from_evidence(self):
        with mock.patch.dict(os.environ,dict(GITHUB_SHA='a'*40,GITHUB_RUN_ID='101',GITHUB_RUN_ATTEMPT='2')):
            with self.assertRaisesRegex(ValueError,'wrong CI identity workflow_sha'):c4.verify_output(self.out)
    def test_ci_dirty_source(self):
        env=self.ci_fixture();self.d['working_tree']='local'
        (self.out/'worktree.log').write_text('local');self.seal()
        with mock.patch.dict(os.environ,env):
            with self.assertRaisesRegex(ValueError,'dirty/wrong CI source'):c4.verify_output(self.out)
    def test_ci_wrong_source_commit(self):
        env=self.ci_fixture();self.d['commit_sha']='b'*40
        (self.out/'commit.log').write_text('b'*40+'\n');self.seal()
        with mock.patch.dict(os.environ,env):
            with self.assertRaisesRegex(ValueError,'dirty/wrong CI source'):c4.verify_output(self.out)
    def test_failed_checkpoint(self):self.d['status']='failed';self.reject()
    def test_promotion(self):self.d['promotion']=True;self.reject()
    def test_missing_capacity_profile(self):self.d['samples'].pop();self.reject()
    def test_duplicate_capacity_profile(self):self.d['samples'][1]=self.d['samples'][0];self.reject()
    def test_nonzero_exit(self):self.d['samples'][0]['returncode']=1;self.reject()
    def test_deadline(self):self.d['samples'][0]['deadline_expired']=True;self.reject()
    def test_changed_command(self):
        (self.out/self.d['samples'][0]['command']).write_text('[]');self.reject()
    def test_raw_tamper(self):
        with (self.out/self.d['samples'][0]['events']).open('a') as stream:stream.write('{}\n')
        self.reject(False)
    def test_missing_raw(self):
        (self.out/self.d['samples'][0]['events']).unlink();self.reject()
    def test_foreign_external_pid(self):
        p=self.out/self.d['samples'][0]['rss'];p.write_text(p.read_text().replace('123','124'));self.reject()
    def test_short_supervisor(self):self.d['samples'][0]['wall_seconds']=1;self.reject()
    def test_short_utc(self):self.d['samples'][0]['finished_utc']='2026-11-17T00:00:01+00:00';self.reject()
    def test_binary_identity(self):self.d['binary_sha256']='b'*64;self.reject()
    def test_commit_drift(self):self.d['commit_sha']='b'*40;self.reject()
    def test_worktree_drift(self):self.d['working_tree']='different';self.reject()
    def test_compiler_drift(self):self.d['compiler_version']='18.0.0';self.reject()
    def test_source_binding(self):self.d['source_inputs'][c4.INPUTS[0]]='b'*64;self.reject()
    def test_instrumentation_disabled(self):
        p=self.out/'CMakeCache.txt';p.write_text(p.read_text().replace('KCP_VALIDATION:BOOL=ON','KCP_VALIDATION:BOOL=OFF'));self.reject()
    def test_sanitizer_diagnostic(self):
        (self.out/self.d['samples'][0]['stderr']).write_text('WARNING: ThreadSanitizer: data race');self.reject()
    def test_missing_terminal_rss_is_explicit(self):
        p=self.out/self.d['samples'][0]['rss']
        with p.open('a') as stream:stream.write(json.dumps(dict(pid=123,elapsed_seconds=2.05,rss_bytes=None,peak_rss_bytes=None,reason='process-exited'))+'\n')
        self.seal();c4.verify_output(self.out)
    def test_missing_live_rss_rejected(self):
        p=self.out/self.d['samples'][0]['rss']
        p.write_text(json.dumps(dict(pid=123,elapsed_seconds=0.1,rss_bytes=None,peak_rss_bytes=None,reason='process-exited'))+'\n')
        self.reject()
    def test_child_killed_and_joined_on_malformed_output(self):
        child_script=self.out/'bad-child.py'
        child_script.write_text('import time\nprint("malformed",flush=True)\ntime.sleep(120)\n')
        original=subprocess.Popen; children=[]
        def spawn(command,**kwargs):
            child=original([sys.executable,str(child_script)],**kwargs);children.append(child);return child
        with mock.patch.object(c4.subprocess,'Popen',side_effect=spawn):
            with self.assertRaises(ValueError):c4.run_sample('not-used',1,4,2,self.out,os.environ.copy())
        self.assertIsNotNone(children[0].poll())
        self.assertTrue(children[0].stdout.closed)
        checkpoint=json.loads((self.out/'n1-family4-checkpoint.json').read_text())
        self.assertEqual(checkpoint['pid'],children[0].pid)
        self.assertNotIn('proof',checkpoint)

if __name__=='__main__':unittest.main()
