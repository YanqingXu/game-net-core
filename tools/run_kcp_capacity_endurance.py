# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Run and independently read back bounded, non-promoting KCP-C4 development evidence."""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time

SCHEMA = 'gamenet.kcp_capacity_endurance.v1'
CHILD_SCHEMA = 'gamenet.kcp_validation.child.v1'
MIB = 1048576
REPO = Path(__file__).resolve().parents[1]
INPUTS = ['CMakeLists.txt', 'benchmarks/kcp_validation/CMakeLists.txt',
          'benchmarks/kcp_validation/main.cpp', 'tools/run_kcp_capacity_endurance.py',
          'intents/usecases/kcp_capacity_endurance.intent.md',
          'rules/thread_affinity_rules.md', 'rules/ownership_rules.md', 'rules/testing_rules.md']


def expected_counts(n):
    healthy = int(n > 1)
    return dict(accepted=4*n+2+healthy, acknowledged=4*n+healthy, cancelled=1, failed=1,
                received=4*n+healthy, peer_accepted=4*n+healthy,
                peer_acknowledged=4*n+healthy, peer_received=4*n+healthy,
                session_rejections=2, pending_rejections=n, memory_rejections=1,
                history_rejections=1, stale_rejections=2, closing_charged=1,
                healthy_progress=healthy)


def demand(condition, message):
    if not condition:
        raise ValueError(message)


def number(row, key, minimum=0):
    value = row.get(key)
    demand(type(value) is int and value >= minimum, 'missing/invalid integer: '+key)
    return value


def validate_events(records, n, family, duration_seconds, pid, sanitized=False):
    """Validate raw observations; totals and limits are derived here, never trusted."""
    demand(n in (1, 8, 32) and family in (0, 4, 6), 'profile')
    demand(len(records) >= 3, 'truncated child stream')
    ready, final = records[0], records[-1]
    demand(ready.get('type') == 'ready' and ready.get('schema') == CHILD_SCHEMA, 'child schema')
    demand(ready.get('backend') in ('iocp', 'epoll'), 'native backend')
    for k, v in dict(sessions=n, family=family, duration_ms=duration_seconds*1000,
                     hub_protocol_limit=n*MIB, peer_protocol_limit=MIB,
                     pending_limit=2*n, pending_byte_limit=4096*n, max_proxy_datagrams=4).items():
        demand(number(ready, k) == v, 'reconfigured '+k)
    demand(0 < number(ready, 'conv_base') <= 0x7fffffff, 'conv namespace')
    demand(final.get('type') == 'final' and final.get('status') == 'passed', 'incomplete run')
    demand(number(final, 'residual') == 0, 'final residual')
    rows = records[1:-1]
    demand(number(final, 'cycles', 1) == len(rows), 'cycle count')
    elapsed, peak, families = 0, 0, set()
    for row in records:
        demand(number(row, 'pid', 1) == pid, 'child process replacement')
        current = number(row, 'rss_bytes', 1)
        observed_peak = number(row, 'peak_rss_bytes', 1)
        demand(observed_peak >= max(current, peak), 'nonmonotonic RSS high water')
        if not sanitized:
            demand(observed_peak <= 512*MIB, 'Release peak RSS ceiling')
        peak = observed_peak
    for cycle, row in enumerate(rows, 1):
        demand(row.get('type') == 'heartbeat' and number(row, 'cycle', 1) == cycle, 'heartbeat sequence')
        demand(number(row, 'sessions') == n, 'session profile drift')
        expected_family = family or (4 if cycle % 2 else 6)
        demand(number(row, 'family') == expected_family, 'family coverage/order')
        families.add(row['family'])
        t = number(row, 'elapsed_ms', 1)
        demand(t > elapsed, 'nonmonotonic heartbeat time')
        elapsed = t
        for k, v in expected_counts(n).items():
            demand(number(row, k) == v, 'contract count: '+k)
        demand(number(row, 'residual') == 0, 'cycle residual')
        demand(number(row, 'wire_drops') >= n and number(row, 'ack_drops') == n, 'missing wire impairment')
        arena = number(row, 'arena_bytes', 1)
        demand(2*n*arena <= number(row, 'peak_protocol_bytes') <= 2*n*MIB, 'protocol memory budget')
        demand(number(row, 'peak_pending_messages') == 2*n, 'pending pressure')
        demand(2048*n <= number(row, 'peak_pending_bytes') <= 4096*n, 'pending byte budget')
        demand(0 < number(row, 'proxy_peak_datagrams') <= 4, 'proxy native queue budget')
    demand(number(final, 'elapsed_ms') >= max(elapsed, duration_seconds*1000), 'shortened duration')
    if not family:
        demand(families == {4, 6}, 'missing dual-family cycles')
    if not sanitized:
        demand(final['rss_bytes'] <= rows[0]['rss_bytes']+64*MIB, 'post-warmup retained RSS growth')
    return dict(cycles=len(rows), elapsed_ms=final['elapsed_ms'], families=sorted(families),
                peak_rss_bytes=peak, settled_rss_bytes=final['rss_bytes'], residual=0)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def hashes(output):
    files = {}
    for path in output.rglob('*'):
        demand(not path.is_symlink(), 'evidence symlink')
        if path.is_file() and path != output/'manifest.json':
            files[path.relative_to(output).as_posix()] = digest(path)
    return files


def load_lines(path):
    # Duplicate JSON keys can hide changed fields from a normal JSON parser.
    def unique(pairs):
        result = {}
        for key, value in pairs:
            demand(key not in result, 'duplicate JSON key')
            result[key] = value
        return result
    return [json.loads(line, object_pairs_hook=unique) for line in path.read_text(encoding='utf-8').splitlines()]


def verify_output(output):
    output = Path(output)
    d = json.loads((output/'manifest.json').read_text(encoding='utf-8'))
    demand(d.get('schema') == SCHEMA and d.get('status') == 'passed', 'incomplete C4 evidence')
    demand(d.get('promotion') is False and d.get('evidence_class') == 'development', 'promotion/class')
    demand(d.get('profile') in ('release', 'asan', 'tsan'), 'sanitizer profile')
    demand(re.fullmatch('[0-9a-f]{40}', d.get('commit_sha', '')), 'source commit')
    demand((output/'commit.log').read_text(encoding='utf-8').strip() == d['commit_sha'], 'raw source commit drift')
    demand((output/'worktree.log').read_text(encoding='utf-8') == d.get('working_tree'), 'raw worktree drift')
    for key, env in [('workflow_sha', 'GITHUB_SHA'), ('run_id', 'GITHUB_RUN_ID'), ('run_attempt', 'GITHUB_RUN_ATTEMPT')]:
        if os.getenv(env):
            demand(d.get(key) == os.getenv(env), 'wrong CI identity '+key)
    if d.get('workflow_sha'):
        demand(d['workflow_sha'] == d['commit_sha'] and not d.get('working_tree'), 'dirty/wrong CI source')
    demand(hashes(output) == d.get('files'), 'evidence inventory/hash mismatch')
    demand(all(digest(output/'inputs'/p) == sha for p, sha in d.get('source_inputs', {}).items())
           and set(d.get('source_inputs', {})) == set(INPUTS), 'source inputs')
    source = json.loads((output/'source.json').read_text(encoding='utf-8'))
    demand(source and all(re.fullmatch('[0-9a-f]{64}', v) for v in source.values()), 'source inventory')
    demand(all(source.get(p) == sha for p, sha in d['source_inputs'].items()), 'source binding')
    demand(d.get('binary_sha256') == digest(output/'native-tool.bin'), 'binary identity')
    expected = [(n, f) for n in (1, 8, 32) for f in (4, 6)] if d.get('mode') == 'smoke' else [(32, 0)]
    demand(d.get('mode') in ('smoke', 'soak'), 'mode')
    duration = 2 if d['mode'] == 'smoke' else 1800
    samples = d.get('samples', [])
    demand(len(samples) == len(expected), 'missing/restarted sample')
    for sample, (n, family) in zip(samples, expected):
        demand((sample.get('sessions'), sample.get('family'), sample.get('duration_seconds')) == (n, family, duration), 'sample configuration')
        demand(sample.get('returncode') == 0 and sample.get('deadline_expired') is False, 'child exit/deadline')
        events = load_lines(output/sample['events'])
        command = json.loads((output/sample['command']).read_text(encoding='utf-8'))
        demand(command[1:] == ['--sessions', str(n), '--family', str(family), '--duration-seconds', str(duration),
                               '--conv-base', str(events[0].get('conv_base'))], 'executed command drift')
        proof = validate_events(events, n, family, duration, sample['pid'], d['profile'] != 'release')
        demand(proof == sample.get('proof'), 'derived proof drift')
        demand(sample.get('wall_seconds', 0) >= duration, 'supervisor duration')
        started = datetime.fromisoformat(sample.get('started_utc', ''))
        finished = datetime.fromisoformat(sample.get('finished_utc', ''))
        demand(started.utcoffset() == finished.utcoffset() == timezone.utc.utcoffset(None)
               and (finished-started).total_seconds() >= duration, 'supervisor UTC interval')
        rss_rows = load_lines(output/sample['rss'])
        demand(len(rss_rows) >= 2, 'missing external RSS observations')
        previous, positive_time, terminal = -1, -1, False
        for row in rss_rows:
            demand(number(row, 'pid', 1) == sample['pid'], 'external process identity')
            t = row.get('elapsed_seconds', -1)
            demand(isinstance(t, (int, float)) and t > previous, 'external sample clock')
            previous = t
            if row.get('rss_bytes') is None:
                demand(row.get('peak_rss_bytes') is None and row.get('reason') == 'process-exited'
                       and positive_time >= 0 and sample['wall_seconds']-1 <= t <= sample['wall_seconds'],
                       'unexplained/nonterminal missing RSS')
                terminal = True
                continue
            demand(not terminal, 'RSS measurement after process exit')
            positive_time = t
            demand(number(row, 'rss_bytes', 1) <= number(row, 'peak_rss_bytes', 1), 'external RSS')
            if d['profile'] == 'release':
                demand(row['peak_rss_bytes'] <= 512*MIB, 'external Release RSS ceiling')
        demand(positive_time >= duration-0.5, 'external observations truncated')
        err = (output/sample['stderr']).read_text(encoding='utf-8', errors='replace')
        demand(not re.search(r'ERROR: (AddressSanitizer|ThreadSanitizer)|WARNING: ThreadSanitizer|runtime error:', err), 'sanitizer report')
    cache = (output/'CMakeCache.txt').read_text(encoding='utf-8')
    for flag in ('GAMENET_ENABLE_UDP', 'GAMENET_ENABLE_KCP', 'GAMENET_BUILD_KCP_VALIDATION'):
        demand(flag+':BOOL=ON' in cache, 'build flag '+flag)
    for flag, on in [('GAMENET_ENABLE_ASAN_UBSAN', d['profile'] == 'asan'), ('GAMENET_ENABLE_TSAN', d['profile'] == 'tsan')]:
        demand(flag+':BOOL='+('ON' if on else 'OFF') in cache, 'instrumentation configuration')
    compilation = (output/'compilation.json').read_text(encoding='utf-8')
    compiler = (output/'compiler.cmake').read_text(encoding='utf-8')
    for field in ('ID', 'VERSION'):
        match = re.search(r'set\(CMAKE_CXX_COMPILER_'+field+r' "([^"]+)"\)', compiler)
        demand(match and match[1] == d.get('compiler_'+field.lower()), 'configured compiler '+field)
    if d['profile'] != 'release':
        flag = ('/fsanitize=address' if d['platform'] == 'Windows' else '-fsanitize=address,undefined') if d['profile'] == 'asan' else '-fsanitize=thread'
        demand(flag in compilation, 'missing tool instrumentation')
        demand(not re.search(r'-fno-sanitize|sanitize-ignorelist|suppressions=', compilation), 'sanitizer bypass')
    return d


def external_rss(pid):
    if os.name == 'nt':
        from ctypes import wintypes
        class Counters(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)]+[(name, ctypes.c_size_t) for name in (
                'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage',
                'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        psapi = ctypes.WinDLL('psapi', use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        handle = kernel.OpenProcess(0x410, False, pid)
        if not handle: raise OSError('OpenProcess RSS')
        try:
            c = Counters(); c.cb = ctypes.sizeof(c)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb): raise OSError('GetProcessMemoryInfo')
            return c.WorkingSetSize, c.PeakWorkingSetSize
        finally:
            kernel.CloseHandle(handle)
    values = {}
    for line in Path('/proc', str(pid), 'status').read_text().splitlines():
        if line.startswith(('VmRSS:', 'VmHWM:')):
            key, value, _ = line.split(); values[key] = int(value)*1024
    return values['VmRSS:'], values['VmHWM:']


def run_sample(binary, n, family, duration, out, env):
    stem = f'n{n}-family{family}'
    sample = dict(sessions=n, family=family, duration_seconds=duration, events=stem+'.jsonl',
                  stderr=stem+'.stderr.log', rss=stem+'.rss.jsonl', command=stem+'-command.json',
                  deadline_expired=False, started_utc=datetime.now(timezone.utc).isoformat())
    command = [str(binary), '--sessions', str(n), '--family', str(family), '--duration-seconds', str(duration),
               '--conv-base', str(secrets.randbelow(0x7ffff000)+1)]
    (out/sample['command']).write_text(json.dumps(command)+'\n', encoding='utf-8')
    start = time.monotonic(); last = start
    messages = queue.Queue(maxsize=32)
    with (out/sample['stderr']).open('wb') as err, (out/sample['events']).open('wb') as log, (out/sample['rss']).open('w', encoding='utf-8') as rss_log:
        child = subprocess.Popen(command, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=err)
        sample['pid'] = child.pid
        def reader():
            try:
                for line in child.stdout:
                    messages.put(line)
            finally:
                child.stdout.close()
                messages.put(None)
        thread = threading.Thread(target=reader, daemon=True); thread.start()
        try:
            finished = False
            while not finished:
                now = time.monotonic()
                if now-start > duration+120 or now-last > 60:
                    sample['deadline_expired'] = True
                    raise RuntimeError('child/heartbeat deadline')
                try:
                    current, peak = external_rss(child.pid)
                    if current and peak:
                        rss_log.write(json.dumps(dict(pid=child.pid, elapsed_seconds=now-start, rss_bytes=current, peak_rss_bytes=peak))+'\n'); rss_log.flush()
                except (OSError, KeyError, ProcessLookupError):
                    # Linux can remove VmRSS/VmHWM before waitpid reports the exit.
                    # Accept missing data only after confirming termination; preserve
                    # a null observation and its reason instead of a fabricated zero.
                    child.wait(timeout=0.5)
                    rss_log.write(json.dumps(dict(pid=child.pid, elapsed_seconds=now-start,
                        rss_bytes=None, peak_rss_bytes=None, reason='process-exited'))+'\n')
                    rss_log.flush()
                try:
                    line = messages.get(timeout=0.1)
                except queue.Empty:
                    continue
                if line is None:
                    finished = True
                else:
                    log.write(line); log.flush()
                    row = json.loads(line)
                    demand(row.get('type') in ('ready', 'heartbeat', 'final'), 'child record')
                    last = time.monotonic()
            sample['returncode'] = child.wait(timeout=10)
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=10)
            thread.join(timeout=5)
            sample['wall_seconds'] = time.monotonic()-start
            sample['finished_utc'] = datetime.now(timezone.utc).isoformat()
            (out/(stem+'-checkpoint.json')).write_text(json.dumps(sample, indent=2)+'\n', encoding='utf-8')
    demand(sample['returncode'] == 0, 'child failed: '+sample['stderr'])
    sample['proof'] = validate_events(load_lines(out/sample['events']), n, family, duration, child.pid, env.get('GAMENET_C4_SANITIZED') == '1')
    return sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=['release', 'asan', 'tsan'])
    parser.add_argument('--mode', choices=['smoke', 'soak'], default='smoke')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--compiler')
    parser.add_argument('--verify-output', type=Path)
    a = parser.parse_args()
    if a.verify_output:
        verify_output(a.verify_output); print('KCP-C4 evidence readback passed'); return
    if not (a.profile and a.build_dir and a.output): parser.error('profile/build-dir/output required')
    if a.mode == 'soak' and a.profile != 'release': parser.error('continuous development profile is Release')
    build, out = a.build_dir.resolve(), a.output.resolve()
    demand(not out.exists() or not any(out.iterdir()), 'fresh output directory required')
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if a.profile == 'asan': env.update(ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', GAMENET_C4_SANITIZED='1')
    if a.profile == 'tsan': env.update(TSAN_OPTIONS='halt_on_error=1', GAMENET_C4_SANITIZED='1')
    d = dict(schema=SCHEMA, status='failed', evidence_class='development', promotion=False,
             profile=a.profile, mode=a.mode, platform=platform.system(), samples=[],
             workflow_sha=env.get('GITHUB_SHA'), run_id=env.get('GITHUB_RUN_ID'), run_attempt=env.get('GITHUB_RUN_ATTEMPT'))
    def run(name, command):
        (out/(name+'-command.json')).write_text(json.dumps(command, indent=2)+'\n', encoding='utf-8')
        print('Running '+name, flush=True)
        with (out/(name+'.log')).open('wb') as stream:
            r = subprocess.run(command, cwd=REPO, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=1800)
        demand(r.returncode == 0, name+' failed; see '+str(out/(name+'.log')))
        return (out/(name+'.log')).read_text(encoding='utf-8', errors='replace')
    try:
        d['commit_sha'] = run('commit', ['git', 'rev-parse', 'HEAD']).strip()
        d['working_tree'] = run('worktree', ['git', 'status', '--porcelain=v1'])
        paths = run('source-paths', ['git', 'ls-files', '--cached', '--others', '--exclude-standard']).splitlines()
        source = {p: digest(REPO/p) for p in paths if p != 'debug.log' and (REPO/p).is_file()}
        (out/'source.json').write_text(json.dumps(source, indent=2, sort_keys=True)+'\n', encoding='utf-8')
        d['source_inputs'] = {p: digest(REPO/p) for p in INPUTS}
        for p in INPUTS:
            dest = out/'inputs'/p; dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(REPO/p, dest)
        config = 'Release' if a.profile == 'release' else 'Debug'
        command = ['cmake', '-S', str(REPO), '-B', str(build), '-DCMAKE_BUILD_TYPE='+config,
                   '-DGAMENET_ENABLE_KCP=ON', '-DGAMENET_ENABLE_UDP=ON', '-DGAMENET_BUILD_KCP_VALIDATION=ON',
                   '-DGAMENET_ENABLE_TLS=OFF', '-DGAMENET_ENABLE_WEBSOCKET=OFF', '-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON',
                   '-DGAMENET_BUILD_TESTING=ON', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                   '-DGAMENET_ENABLE_ASAN_UBSAN='+('ON' if a.profile == 'asan' else 'OFF'),
                   '-DGAMENET_ENABLE_TSAN='+('ON' if a.profile == 'tsan' else 'OFF')]
        command += ['-A', 'x64'] if os.name == 'nt' else ['-G', 'Ninja']
        if a.compiler: command += ['-DCMAKE_CXX_COMPILER='+a.compiler]
        run('configure', command)
        shutil.copyfile(build/'CMakeCache.txt', out/'CMakeCache.txt')
        compiler_files = list((build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
        demand(len(compiler_files) == 1, 'ambiguous compiler configuration')
        shutil.copyfile(compiler_files[0], out/'compiler.cmake')
        compiler_text = compiler_files[0].read_text(encoding='utf-8')
        for field in ('ID', 'VERSION'):
            match = re.search(r'set\(CMAKE_CXX_COMPILER_'+field+r' "([^"]+)"\)', compiler_text)
            demand(match, 'missing compiler '+field)
            d['compiler_'+field.lower()] = match[1]
        if os.name == 'nt':
            for compiler in (build/'CMakeFiles').rglob('CMakeCXXCompiler.cmake'):
                match = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)', compiler.read_text(encoding='utf-8'))
                if match: env['PATH'] = str(Path(match[1]).parent)+os.pathsep+env.get('PATH', '')
            compilation = {str(p.relative_to(build)): p.read_text(encoding='utf-8') for p in (build/'benchmarks/kcp_validation').glob('*.vcxproj')}
        else:
            compilation = [entry for entry in json.loads((build/'compile_commands.json').read_text()) if entry['file'].endswith('benchmarks/kcp_validation/main.cpp')]
        demand(bool(compilation), 'native compilation evidence')
        (out/'compilation.json').write_text(json.dumps(compilation, indent=2)+'\n', encoding='utf-8')
        run('build', ['cmake', '--build', str(build), '--config', config, '--parallel', '2', '--target', 'gamenet_kcp_validation'])
        binary = build/'benchmarks/kcp_validation'
        binary = binary/config/'gamenet_kcp_validation.exe' if os.name == 'nt' else binary/'gamenet_kcp_validation'
        shutil.copyfile(binary, out/'native-tool.bin'); d['binary_sha256'] = digest(binary)
        profiles = [(n, f) for n in (1, 8, 32) for f in (4, 6)] if a.mode == 'smoke' else [(32, 0)]
        for n, family in profiles:
            print(f'Running C4 {n=} {family=} {a.mode}', flush=True)
            d['samples'].append(run_sample(binary, n, family, 2 if a.mode == 'smoke' else 1800, out, env))
        demand(all(digest(REPO/p) == sha for p, sha in source.items()), 'source changed during validation')
        d['status'] = 'passed'
    except BaseException as error:
        d['error'] = str(error)
        raise
    finally:
        d['files'] = hashes(out)
        (out/'manifest.json').write_text(json.dumps(d, indent=2)+'\n', encoding='utf-8')
    verify_output(out)
    print('KCP-C4 '+a.mode+' passed (development evidence only)', flush=True)


if __name__ == '__main__':
    main()
