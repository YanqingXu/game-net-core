# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Native GameNet server against an independently linked unchanged upstream peer."""
from __future__ import annotations
import heapq,json,queue,select,socket,struct,subprocess,threading,time
from pathlib import Path

EXPECTED={'implementation':'unmodified-upstream','families':[4,6],'messages':8,
          'data_loss_directions':4,'ack_loss_directions':4,'duplicate_directions':4,
          'reorder_directions':4,'bounded_proxy':128,'zero_residue':True,'status':'passed'}

def verify_settled_reset(process):
    # Windows can report delayed ICMP port-unreachable after a peer closes its
    # socket normally. Accept it only after that exact peer exits successfully;
    # its strict native/settlement summary is still checked below.
    try:code=process.wait(timeout=0.25)
    except subprocess.TimeoutExpired as error:raise ValueError('UDP reset while KCP peer is active') from error
    if code!=0:raise ValueError('UDP reset from failed KCP peer')

def verify_evidence(output):
    output=Path(output)
    for family in (4,6):
        proxy=json.loads((output/f'{family}-proxy.json').read_text(encoding='utf-8'))
        if proxy.get('drops')!=[[True,True],[True,True]] or proxy.get('duplicates')!=[True,True] or proxy.get('reordered')!=[True,True]:
            raise ValueError('KCP raw impairment coverage missing')
        if not 1<=proxy.get('peak',0)<=128 or len(proxy.get('forwarded',[]))!=2 or not all(n>0 for n in proxy['forwarded']):
            raise ValueError('KCP raw proxy bounds mismatch')
        expected=[{'status':'passed','received':4,'acknowledged':4,'closed':1,'stopped':1,'residual':0},
                  {'implementation':'unmodified-upstream','status':'passed','messages':4}]
        if proxy.get('summaries')!=expected:raise ValueError('KCP raw proxy settlement mismatch')
        for index,name in enumerate(('server','reference')):
            log=(output/f'{family}-{name}.log').read_text(encoding='utf-8')
            values=[json.loads(s) for s in log.splitlines() if s.startswith('{')]
            if values!=[expected[index]]:raise ValueError('KCP raw peer oracle mismatch')
            if any(s in log for s in ['ERROR:','runtime error:','DEADLYSIGNAL']):raise ValueError('KCP interop sanitizer diagnostic')

def interoperate(server,reference,output,env):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    for family in (4,6):
        host='::1' if family==6 else '127.0.0.1';af=socket.AF_INET6 if family==6 else socket.AF_INET
        sockets=[socket.socket(af,socket.SOCK_DGRAM) for _ in range(2)]
        for s in sockets:s.bind((host,0));s.setblocking(False)
        processes=[];readers=[];queues=[]
        conv=80100+family
        try:
            for name,binary,index in [('server',server,0),('reference',reference,1)]:
                cmd=[str(binary),str(family),str(sockets[index].getsockname()[1]),str(conv)]
                (output/f'{family}-{name}-command.json').write_text(json.dumps(cmd,indent=2)+'\n',encoding='utf-8')
                p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',env=env)
                processes.append(p);q=queue.Queue();queues.append(q)
                def read(proc=p,items=q,path=output/f'{family}-{name}.log'):
                    with path.open('w',encoding='utf-8') as f:
                        for line in proc.stdout:f.write(line);f.flush();items.put(line.strip())
                t=threading.Thread(target=read,daemon=True);t.start();readers.append(t)
            addresses=[]
            for q in queues:
                line=q.get(timeout=15)
                if not line.startswith('PORT '):raise ValueError('peer did not announce port: '+line)
                addresses.append((host,int(line[5:])))
            pending=[];serial=0;packet=[0,0];drops=[[False,False],[False,False]];duplicates=[False,False];reordered=[False,False]
            peak=0;counts=[0,0];post_exit_resets=[0,0];deadline=time.monotonic()+25
            while time.monotonic()<deadline:
                for source in select.select(sockets,[],[],0.001)[0]:
                    direction=sockets.index(source)
                    try:data,peer=source.recvfrom(65536)
                    except ConnectionResetError as error:
                        if getattr(error,'winerror',None)!=10054:raise
                        verify_settled_reset(processes[direction]);post_exit_resets[direction]+=1;continue
                    if peer[:2]!=addresses[direction]:raise ValueError('unexpected interop UDP source')
                    if not 24<=len(data)<=1200:raise ValueError('nonstandard KCP datagram size')
                    at=0;commands=[]
                    while at<len(data):
                        if len(data)-at<24:raise ValueError('partial KCP header')
                        c,cmd,frg,wnd,ts,sn,una,n=struct.unpack_from('<IBBHIIII',data,at)
                        if c!=conv or cmd not in (81,82,83,84) or at+24+n>len(data):raise ValueError('nonstandard KCP wire')
                        commands.append(cmd);at+=24+n
                    packet[direction]+=1
                    if 81 in commands and not drops[direction][0]:drops[direction][0]=True;continue
                    if 82 in commands and not drops[direction][1]:drops[direction][1]=True;continue
                    delay=0.055 if packet[direction]%3==0 else 0.002
                    when=time.monotonic()+delay
                    if any(d==direction and due>when for due,_,d,_ in pending):reordered[direction]=True
                    serial+=1;heapq.heappush(pending,(when,serial,direction,data))
                    if 81 in commands and not duplicates[direction]:
                        duplicates[direction]=True;serial+=1;heapq.heappush(pending,(when+0.008,serial,direction,data))
                    peak=max(peak,len(pending))
                    if peak>128:raise ValueError('impairment proxy exceeded bounded queue')
                now=time.monotonic()
                while pending and pending[0][0]<=now:
                    _,_,direction,data=heapq.heappop(pending);target=1-direction
                    sockets[target].sendto(data,addresses[target]);counts[direction]+=1
                if all(p.poll() is not None for p in processes):break
            for p in processes:
                if p.wait(timeout=2)!=0:raise RuntimeError('independent KCP peer failed')
            for t in readers:t.join(timeout=2)
            summaries=[]
            for q in queues:
                lines=[]
                while not q.empty():lines.append(q.get_nowait())
                values=[json.loads(line) for line in lines if line.startswith('{')]
                if len(values)!=1:raise ValueError('missing independent summary')
                summaries.append(values[0])
            if summaries[0]!={'status':'passed','received':4,'acknowledged':4,'closed':1,'stopped':1,'residual':0}:raise ValueError('server settlement mismatch')
            if summaries[1]!={'implementation':'unmodified-upstream','status':'passed','messages':4}:raise ValueError('upstream message oracle mismatch')
            if not all(all(d) for d in drops) or not all(duplicates) or not all(reordered):raise ValueError('impairment coverage incomplete')
            (output/f'{family}-proxy.json').write_text(json.dumps({'drops':drops,'duplicates':duplicates,'reordered':reordered,'peak':peak,'forwarded':counts,'post_exit_resets':post_exit_resets,'summaries':summaries},indent=2)+'\n',encoding='utf-8')
        finally:
            for p in processes:
                if p.poll() is None:p.kill();p.wait(timeout=5)
            for s in sockets:s.close()
    (output/'summary.json').write_text(json.dumps(EXPECTED,indent=2)+'\n',encoding='utf-8')
    verify_evidence(output);return EXPECTED.copy()
