// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// 双端示例的有限运行入口；不安装，不改变 KCP 协议或公共 API。
namespace gamenet::example {
enum class EchoRole { Server, Client };
int runKcpEcho(EchoRole, int argc, char** argv);
}
