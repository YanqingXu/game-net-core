# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Independent explicit-source UDP peer and local-session replacement checks."""
from __future__ import annotations
import os,queue,re,socket,subprocess,threading
from pathlib import Path

EXPECTED={'implementation':'python.socket','families':[4,6],'generations':8,'datagrams':64,
          'empty_datagrams':16,'truncated_datagrams':16,'rotations':8,'unknown_sources':8,'status':'passed'}

def interoperate(binary:Path,output:Path,environment:dict)->dict:
    output.mkdir(parents=True,exist_ok=True)
    payloads=[b'',b'\x00\xff\x80binary',b'x'*64,b'z'*65,b'y'*512,b'rotate',b'',b'after-rotation']
    counts=dict(EXPECTED)
    for key in ['generations','datagrams','empty_datagrams','truncated_datagrams','rotations','unknown_sources']:counts[key]=0
    for family in [4,6]:
        host='127.0.0.1' if family==4 else '::1'
        for generation in range(4):
            with socket.socket(socket.AF_INET if family==4 else socket.AF_INET6,socket.SOCK_DGRAM) as peer:
                peer.bind((host,0));peer.settimeout(5)
                process=subprocess.Popen([str(binary),str(family),str(peer.getsockname()[1])],stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,env=environment,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
                lines=queue.Queue();path=output/f'peer-{family}-{generation}.log'
                def read(proc=process,sink=lines,dest=path):
                    with dest.open('wb') as log:
                        for line in iter(proc.stdout.readline,b''):
                            log.write(line);log.flush();sink.put(line.decode('utf-8',errors='replace').strip())
                thread=threading.Thread(target=read,daemon=True);thread.start()
                try:
                    port=None
                    for _ in range(30):
                        line=lines.get(timeout=10)
                        if line.startswith('PORT '):port=int(line.split()[1]);break
                    if not port:raise ValueError('peer server port missing')
                    with socket.socket(peer.family,socket.SOCK_DGRAM) as unknown:
                        unknown.bind((host,0));unknown.settimeout(.1);unknown.sendto(b'unknown',(host,port))
                        try:unknown.recvfrom(1024)
                        except socket.timeout:counts['unknown_sources']+=1
                        else:raise ValueError('unregistered peer received output')
                    for payload in payloads:
                        if peer.sendto(payload,(host,port))!=len(payload):raise ValueError('partial UDP send')
                        data,source=peer.recvfrom(1024)
                        expected=b'rotated' if payload==b'rotate' else payload[:64]
                        if data!=expected or source[0]!=host or source[1]!=port:raise ValueError('peer boundary/source/generation mismatch')
                        counts['datagrams']+=1
                        if not payload:counts['empty_datagrams']+=1
                        if len(payload)>64:counts['truncated_datagrams']+=1
                        if payload==b'rotate':counts['rotations']+=1
                    if process.wait(timeout=10):raise ValueError('peer server failure')
                    thread.join(timeout=3)
                    if thread.is_alive():raise ValueError('peer log thread did not stop')
                    log=path.read_text(encoding='utf-8',errors='replace')
                    if re.search(r'ERROR:|runtime error:|WARNING: ThreadSanitizer|DEADLYSIGNAL',log):raise ValueError('peer sanitizer diagnostic')
                    if re.findall(r'^PEER_SUMMARY (.*)$',log,re.M)!=['8 8 2 2 1 0 0 passed']:raise ValueError('peer summary/zero cleanup mismatch')
                    counts['generations']+=1
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:process.wait(timeout=5)
                        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
                    thread.join(timeout=3);process.stdout.close()
    if counts!=EXPECTED:raise ValueError('independent peer count mismatch')
    return counts
