// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/websocket/WebSocketCodec.h>
int main(){gamenet::websocket::WebSocketCodec codec;return codec.push({}).errorCode?1:0;}
