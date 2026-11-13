# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Pinned independent peer: one asyncio owner performs every TLS operation."""
import asyncio


async def exchange(uri,context,generation,trace,*,timeout=3):
    from websockets.asyncio.client import connect
    trace.records.clear()
    async with connect(uri,ssl=context,server_hostname='localhost',proxy=None,compression=None,
                       open_timeout=timeout,close_timeout=timeout,max_size=1024*1024,logger=trace.logger) as ws:
        trace.capture(generation,'connected',ws)
        try:
            text='secure € 😀 '+str(generation)
            await ws.send([text[:3],text[3:]])
            if await asyncio.wait_for(ws.recv(),timeout)!=text:raise ValueError('WSS text mismatch')
            payload=b'binary\0'+bytes(range(256))*273
            await ws.send([payload[:123],payload[123:]])
            if await asyncio.wait_for(ws.recv(),timeout)!=payload:raise ValueError('WSS binary mismatch')
            pong=await ws.ping(b'wss-ping');await asyncio.wait_for(pong,timeout)
            await ws.close(1000,'done')
            trace.capture(generation,'closed',ws)
            if ws.close_code!=1000 or ws.close_reason!='done':raise ValueError('WSS Close(1000, done) missing')
            if ws.protocol.close_sent is None or ws.protocol.close_rcvd is None:raise ValueError('WSS Close frame missing')
        except BaseException as error:
            trace.capture(generation,'failure',ws,error)
            raise
