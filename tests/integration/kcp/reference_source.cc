// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// An independently linked original copy; no symbol shim, arena or GameNet linkage.
#include "../../../third_party/kcp/ikcp.h"
#include <cstddef>
// C++ portability only: the same standard layout offset, without a null lvalue.
#undef iqueue_entry
#define iqueue_entry(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))
#include "../../../third_party/kcp/ikcp.c"
