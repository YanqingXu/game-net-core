# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Bounded diagnostic probe; full WSS contracts remain the qualification gate."""
import argparse, asyncio, hashlib, json, os, queue, re, ssl, subprocess, sys, threading
from pathlib import Path
from wss_interop_diagnostics import ClientTrace
from wss_async_peer import exchange


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--profile',choices=['release','asan'],required=True)
    parser.add_argument('--generations',type=int,default=256)
    args=parser.parse_args()
    if not 1<=args.generations<=1024:parser.error('generations must be 1..1024')
    build=args.build_dir.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    config='Release' if args.profile=='release' else 'Debug'
    env=os.environ.copy()
    if args.profile=='asan':env['ASAN_OPTIONS']='halt_on_error=1'
    for path in (build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
        match=re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"',path.read_text())
        if match:env['PATH']=str(Path(match[1]).parent)+os.pathsep+env.get('PATH','');break
    binaries=build/'tests'/config
    certs=out/'certificates';certs.mkdir(exist_ok=True)
    generator=next(binaries.glob('gnt_integration_websocket_tls_*.exe'))
    subprocess.run([str(generator),str(certs)],env=env,check=True,timeout=30)
    binary=binaries/'gamenet_websocket_tls_echo_server.exe'
    trace=ClientTrace(out/'client.jsonl');lines=queue.Queue()
    manifest={'schema':'gamenet.wss_close_diagnostic.v1','status':'failed','generations':0,
              'requested_generations':args.generations,'profile':args.profile,
              'commit_sha':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
              'workflow_sha':env.get('GITHUB_SHA'),'run_id':env.get('GITHUB_RUN_ID'),
              'run_attempt':env.get('GITHUB_RUN_ATTEMPT'),
              'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'client_api':'asyncio'}
    process=subprocess.Popen([str(binary),str(certs/'server.pem'),str(certs/'key.pem'),'0','180'],
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env)
    def reader():
        with (out/'server.log').open('wb') as log:
            for line in iter(process.stdout.readline,b''):
                log.write(line);log.flush();lines.put(line.decode('utf-8',errors='replace').strip())
    thread=threading.Thread(target=reader);thread.start()
    try:
        line=lines.get(timeout=10)
        if not line.startswith('PORT '):raise ValueError('missing server port')
        port=int(line.split()[1]);context=ssl.create_default_context(cafile=str(certs/'ca.pem'))
        for generation in range(args.generations):
            asyncio.run(exchange(f'wss://127.0.0.1:{port}/echo',context,generation,trace))
            while True:
                line=lines.get(timeout=5)
                if line.startswith('CLOSED '):
                    if line!='CLOSED 1 1 1':raise ValueError('unexpected layered close: '+line)
                    break
            manifest['generations']+=1
        manifest['status']='passed'
    except Exception as error:
        manifest['error']=repr(error)
        raise
    finally:
        # Diagnostic-only process retirement, after observed per-connection close.
        # Normal shutdown/sanitizer qualification uses run_websocket_tls_contracts.
        if process.poll() is None:process.terminate()
        process.wait(timeout=10);thread.join(timeout=5);process.stdout.close()
        manifest['process_retirement']='diagnostic supervisor termination'
        manifest['files']={p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in out.rglob('*') if p.is_file() and p.name!='manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':main()
