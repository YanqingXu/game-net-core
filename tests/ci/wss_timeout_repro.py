# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Real TLS record barrier reproducing the legacy SSLSocket timeout race."""
import json,socket,ssl,threading,time


def verify_timeout_proof(result):
    if result.get('status')!='legacy_false_timeout_reproduced' or result.get('timeout_at_header','missing') is not None or \
            result.get('new_timeout_seconds')!=3 or result.get('exception_type')!='TimeoutError' or \
            result.get('failed_before_body_release') is not True or result.get('worker_errors')!=[] or result.get('elapsed_ns',0)<=0:
        raise ValueError('incomplete real TLS timeout-race proof')


def reproduce(certificates,output):
    ready=threading.Event();armed=threading.Event();header=threading.Event()
    flipped=threading.Event();body=threading.Event();finished=threading.Event()
    result={};errors=[];peer=None;reader=None
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(5)
    def server():
        try:
            with listener.accept()[0] as conn:
                conn.settimeout(5)
                context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(str(certificates/'server.pem'),str(certificates/'key.pem'))
                incoming=ssl.MemoryBIO();outgoing=ssl.MemoryBIO()
                tls=context.wrap_bio(incoming,outgoing,server_side=True)
                while True:
                    try:tls.do_handshake();done=True
                    except ssl.SSLWantReadError:done=False
                    pending=outgoing.read()
                    if pending:conn.sendall(pending)
                    if done:break
                    incoming.write(conn.recv(16384))
                ready.set()
                if not armed.wait(5):raise TimeoutError('reader did not arm')
                tls.write(b'x');record=outgoing.read()
                if len(record)<=5:raise ValueError('missing real TLS record body')
                conn.sendall(record[:5])
                if not body.wait(5):raise TimeoutError('body gate not released')
                conn.sendall(record[5:])
        except BaseException as error:
            errors.append(repr(error));ready.set();header.set()
    worker=threading.Thread(target=server);worker.start()
    context=ssl.create_default_context(cafile=str(certificates/'ca.pem'))
    def message(connection,direction,version,content_type,message_type,data):
        if armed.is_set() and not header.is_set() and direction=='read' and int(content_type)==256:
            result['timeout_at_header']=connection.gettimeout()
            header.set()
            if not flipped.wait(5):raise TimeoutError('timeout mode gate not released')
    context._msg_callback=message # test-only hook inside the real SSL_read call
    try:
        peer=context.wrap_socket(socket.create_connection(listener.getsockname(),timeout=5),server_hostname='localhost')
        peer.settimeout(None)
        if not ready.wait(5) or errors:raise ValueError('TLS fixture handshake failed: '+repr(errors))
        def read():
            begin=time.perf_counter_ns()
            try:result['data']=repr(peer.recv(1))
            except BaseException as error:
                result['exception_type']=type(error).__name__;result['exception']=str(error)
            result['elapsed_ns']=time.perf_counter_ns()-begin;finished.set()
        armed.set();reader=threading.Thread(target=read);reader.start()
        if not header.wait(5) or errors:raise ValueError('real TLS header barrier missing')
        peer.settimeout(3);result['new_timeout_seconds']=peer.gettimeout();flipped.set()
        result['failed_before_body_release']=finished.wait(1)
    finally:
        armed.set();flipped.set();body.set()
        if reader:reader.join(timeout=5)
        worker.join(timeout=5)
        if peer:peer.close()
        listener.close()
    result['worker_errors']=errors
    if worker.is_alive() or reader and reader.is_alive():raise ValueError('TLS proof worker did not retire')
    if result.get('timeout_at_header','missing') is not None or result.get('exception_type')!='TimeoutError' or \
            not result.get('failed_before_body_release') or errors:
        raise ValueError('legacy race was not reproduced at the controlled record boundary: '+repr(result))
    result['status']='legacy_false_timeout_reproduced'
    verify_timeout_proof(result)
    output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result
