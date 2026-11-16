// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "EchoExample.h"
int main(int argc, char** argv) {
    return gamenet::example::runKcpEcho(gamenet::example::EchoRole::Server, argc, argv);
}
