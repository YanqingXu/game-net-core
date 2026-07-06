#pragma once

// TestAssert 是测试专用断言工具，确保合同测试在 Release/NDEBUG 下仍会执行。
// 它只用于 tests/，不进入公开库 API。

#include <cstdlib>
#include <iostream>

namespace gamenet::test {

[[noreturn]] inline void fail(const char* expression, const char* file, int line) {
    std::cerr << file << ':' << line << ": test assertion failed: " << expression << '\n';
    std::abort();
}

}  // namespace gamenet::test

#define GAMENET_TEST_ASSERT(expression)                    \
    do {                                                   \
        if (!(expression)) {                               \
            ::gamenet::test::fail(#expression, __FILE__, __LINE__); \
        }                                                  \
    } while (false)

#define GAMENET_TEST_FAIL(message)                         \
    do {                                                   \
        ::gamenet::test::fail((message), __FILE__, __LINE__);       \
    } while (false)
