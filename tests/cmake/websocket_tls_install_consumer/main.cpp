// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#ifdef CONSUME_WSS
#include "gamenet/websocket_tls/WebSocketTlsSession.h"
int main(){gamenet::websocket::WebSocketTlsOptions options;return options.tls.maxPendingWrites?0:1;}
#elif defined(CONSUME_WS)
#include "gamenet/websocket/WebSocketSession.h"
int main(){return gamenet::websocket::WebSocketCodec::validUtf8("installed")?0:1;}
#else
#include "gamenet/core/net/Buffer.h"
int main(){gamenet::net::Buffer b;return b.readableBytes()?1:0;}
#endif
