# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Bounded, non-installed diagnostics for the pinned independent WSS peer."""
from __future__ import annotations

from collections import deque
import json
import logging
import platform
import ssl
import sys
import time


class ClientTrace(logging.Handler):
    def __init__(self, path):
        super().__init__()
        self.path = path
        self.records = deque(maxlen=128)
        self.logger = logging.Logger('gamenet.independent.wss', logging.DEBUG)
        self.logger.addHandler(self)
        self.path.write_text('',encoding='utf-8')
        self.write('runtime', python=sys.version, platform=platform.platform(), openssl=ssl.OPENSSL_VERSION)

    def emit(self, record):
        self.records.append({'time_ns': time.monotonic_ns(), 'message': record.getMessage()[:1024]})

    def write(self, event, **values):
        with self.path.open('a', encoding='utf-8') as output:
            output.write(json.dumps({'event': event, 'time_ns': time.monotonic_ns(), **values}, ensure_ascii=True) + '\n')

    def capture(self, generation, phase, connection, error=None):
        protocol = connection.protocol
        values = dict(generation=generation, phase=phase, close_code=connection.close_code,
                      close_reason=connection.close_reason, state=protocol.state.name,
                      close_sent=repr(protocol.close_sent), close_received=repr(protocol.close_rcvd),
                      close_received_then_sent=protocol.close_rcvd_then_sent,
                      receive_exception=repr(getattr(connection,'recv_exc',None)), parser_exception=repr(protocol.parser_exc),
                      exception=repr(error), trace=list(self.records))
        try:
            if hasattr(connection,'transport'):
                transport=connection.transport;tls=transport.get_extra_info('ssl_object')
                values.update(local=transport.get_extra_info('sockname'),peer=transport.get_extra_info('peername'),
                              tls_version=tls.version() if tls else None,cipher=tls.cipher() if tls else None)
            else:
                values.update(local=connection.socket.getsockname(), peer=connection.socket.getpeername(),
                              tls_version=connection.socket.version(), cipher=connection.socket.cipher())
        except (OSError, ValueError, AttributeError):
            pass
        self.write('connection', **values)
        return values


def verify_peer_diagnostics(folder):
    events=[json.loads(line) for line in (folder/'interop-client.jsonl').read_text(encoding='utf-8').splitlines()]
    connections=[e for e in events if e['event']=='connection']
    connected={e['generation']:e for e in connections if e['phase']=='connected'}
    closed=[e for e in connections if e['phase']=='closed']
    if len(connections)!=16 or len(connected)!=8 or len(closed)!=8 or set(connected)!=set(range(8)):
        raise ValueError('incomplete client generation diagnostics')
    server=[json.loads(line[6:]) for line in (folder/'interop-server.log').read_text(encoding='utf-8').splitlines() if line.startswith('TRACE ')]
    for result in closed:
        if result['close_code']!=1000 or result['close_reason']!='done' or result['receive_exception']!='None' or result['parser_exception']!='None':
            raise ValueError('invalid client close diagnostics')
        if result['close_sent']!="Close(code=1000, reason='done')" or result['close_received']!=result['close_sent']:
            raise ValueError('missing strict Close frame diagnostics')
        frames=[entry['message'] for entry in result['trace']]
        if not any(f.startswith('> CLOSE 1000 ') for f in frames) or not any(f.startswith('< CLOSE 1000 ') for f in frames):
            raise ValueError('missing sent/received Close trace')
        port=connected[result['generation']]['local'][1]
        terminal=[e for e in server if e['event']=='closed' and e['peer_port']==port]
        if len(terminal)!=1:raise ValueError('missing unique correlated server terminal')
        state=terminal[0]
        if state['terminals']!=1 or not all(state[k] for k in ['ws_sent_close','ws_received_close','tls_sent_notify','tls_received_notify']):
            raise ValueError('incomplete layered close diagnostics')
        if any(state[k] for k in ['tcp_pending','ws_controls','ws_input','tls_pending','tls_input','tls_output']):
            raise ValueError('retained bytes in terminal diagnostics')
        if state['tls_accepted']!=state['tls_encrypted']+state['tls_discarded']:
            raise ValueError('TLS terminal ledger mismatch')
    if len([e for e in events if e['event']=='certificate-rejected'])!=3 or [e['status'] for e in events if e['event']=='policy-rejected']!=[403]:
        raise ValueError('missing negative peer diagnostics')
