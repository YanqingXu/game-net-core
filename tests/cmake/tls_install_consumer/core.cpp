// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/core/net/EventLoop.h>
int main() { gamenet::net::EventLoop loop; return loop.isInLoopThread() ? 0 : 1; }
