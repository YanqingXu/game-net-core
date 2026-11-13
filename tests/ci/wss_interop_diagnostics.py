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
                      receive_exception=repr(connection.recv_exc), parser_exception=repr(protocol.parser_exc),
                      exception=repr(error), trace=list(self.records))
        try:
            values.update(local=connection.socket.getsockname(), peer=connection.socket.getpeername(),
                          tls_version=connection.socket.version(), cipher=connection.socket.cipher())
        except (OSError, ValueError, AttributeError):
            pass
        self.write('connection', **values)
        return values
