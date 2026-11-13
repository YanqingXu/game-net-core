# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Independent stdlib UDP peer; no GameNet codec or socket helpers."""
from __future__ import annotations
import argparse,json,os,queue,re,socket,subprocess,threading
from pathlib import Path

def interoperate(binary:Path,output:Path,environment:dict)->dict:
    output.mkdir(parents=True,exist_ok=True)
    counts={'implementation':'python.socket','families':[4,6],'generations':0,'datagrams':0,'empty_datagrams':0,'truncated_datagrams':0,'status':'passed'}
    payloads=[b'',b'\x00\xff\x80binary',b'x'*64,b'z'*65,b'y'*512,b'after-truncation']
    for family in [4,6]:
        for generation in range(4):
            process=subprocess.Popen([str(binary),str(family),'0',str(len(payloads)),'64'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                env=environment,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            lines=queue.Queue();logpath=output/f'peer-{family}-{generation}.log'
            def reader(proc=process,path=logpath,sink=lines):
                with path.open('wb') as log:
                    for line in iter(proc.stdout.readline,b''):
                        log.write(line);log.flush();sink.put(line.decode('utf-8',errors='replace').strip())
            thread=threading.Thread(target=reader,daemon=True);thread.start()
            try:
                port=None
                for _ in range(30):
                    line=lines.get(timeout=10)
                    if line.startswith('PORT '):port=int(line.split()[1]);break
                if not port:raise ValueError('UDP server did not publish bound port')
                host='127.0.0.1' if family==4 else '::1'
                with socket.socket(socket.AF_INET if family==4 else socket.AF_INET6,socket.SOCK_DGRAM) as peer:
                    peer.settimeout(5);peer.bind((host,0))
                    for payload in payloads:
                        if peer.sendto(payload,(host,port))!=len(payload):raise ValueError('partial independent datagram send')
                        received,source=peer.recvfrom(4096)
                        if received!=payload[:64] or source[0]!=host or source[1]!=port:raise ValueError('wrong UDP payload boundary/peer/prefix')
                        counts['datagrams']+=1
                        if not payload:counts['empty_datagrams']+=1
                        if len(payload)>64:counts['truncated_datagrams']+=1
                if process.wait(timeout=10)!=0:raise ValueError('UDP server failed')
                thread.join(timeout=3)
                if thread.is_alive():raise ValueError("UDP peer log reader did not retire")
                log=logpath.read_text(encoding='utf-8',errors='replace')
                if re.search(r'ERROR:|runtime error:|WARNING: ThreadSanitizer|DEADLYSIGNAL',log):raise ValueError('UDP sanitizer diagnostic')
                if re.findall(r'^SUMMARY (.*)$',log,re.M)!=['6 6 2 0']:raise ValueError('wrong UDP server delivery/truncation/settlement ledger')
                counts['generations']+=1
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:process.wait(timeout=5)
                    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
                thread.join(timeout=3)
                process.stdout.close()
    return counts

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();result=interoperate(args.binary.resolve(),args.output,os.environ.copy());print(json.dumps(result,indent=2))
