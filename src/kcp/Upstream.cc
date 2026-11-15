// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// The included file is unchanged MIT upstream source; C++ permits bounded arena
// exhaustion to unwind before upstream's unchecked-null/abort paths.
// clang-format off
#include "KcpSymbols.h"
#include "../../third_party/kcp/ikcp.h"
#include <cstddef>
// Upstream's C offsetof idiom dereferences a null base under C++ UBSan.
// Replace only that macro at compilation, without changing the pinned files.
#undef iqueue_entry
#define iqueue_entry(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))
#include "../../third_party/kcp/ikcp.c"
// clang-format on
