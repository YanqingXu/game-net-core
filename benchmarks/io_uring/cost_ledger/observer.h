// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace cost_ledger {
void begin();
void end();
void save(const char* directory);
}
