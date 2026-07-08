# Coost-Compatible Logger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a C++23 coost-inspired compatibility layer to `gamenet::base::Logger` while preserving existing Reactor/TCP log macros.

**Architecture:** Keep logging synchronous and process-global. Format each record in a temporary `Logger` object, copy configured callbacks under a mutex, then invoke callbacks outside the mutex. Add tests first for the new public macros, topic callback behavior, filtering, and fatal `CHECK` behavior.

**Tech Stack:** C++23, CMake 3.20, CTest, existing `tests/support/TestAssert.h`, Windows/POSIX child-process checks for fatal contract tests.

---

## File Structure

- Modify `include/gamenet/core/base/Logger.h`: keep existing API, add coost-style macros and new callback/reset declarations.
- Modify `src/core/base/Logger.cc`: add thread-safe global state, coost-style formatting, topic output, conditional/error helpers, and reset support.
- Modify `tests/CMakeLists.txt`: register the new base unit and contract tests.
- Create `tests/unit/base/test_logger.cpp`: unit-level behavior for normal logging and macro gating.
- Create `tests/contract/base/test_logger_contract.cpp`: process-level fatal/check behavior and system error context.

## Task 1: Unit Tests For Compatible Logging

**Files:**
- Create: `tests/unit/base/test_logger.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing unit test**

```cpp
#include "gamenet/core/base/Logger.h"
#include "support/TestAssert.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string> records;
std::vector<std::string> topics;

void captureOutput(const char* msg, int len) {
    records.emplace_back(msg, static_cast<std::size_t>(len));
}

void captureTopic(const char* topic, const char* msg, int len) {
    topics.emplace_back(topic ? topic : "");
    records.emplace_back(msg, static_cast<std::size_t>(len));
}

void reset() {
    records.clear();
    topics.clear();
    gamenet::base::Logger::resetForTesting();
    gamenet::base::Logger::setLogLevel(gamenet::base::Logger::TRACE);
    gamenet::base::Logger::setOutputFunction(captureOutput);
    gamenet::base::Logger::setTopicOutputFunction(captureTopic);
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

}  // namespace

int main() {
    reset();
    LOG_INFO << "old macro " << 7;
    GAMENET_TEST_ASSERT(records.size() == 1);
    GAMENET_TEST_ASSERT(contains(records.back(), "old macro 7"));
    GAMENET_TEST_ASSERT(records.back().front() == 'I');

    reset();
    DLOG << "debug " << 42;
    LOG << "info";
    WLOG << "warn";
    ELOG << "error";
    GAMENET_TEST_ASSERT(records.size() == 4);
    GAMENET_TEST_ASSERT(records[0].front() == 'D');
    GAMENET_TEST_ASSERT(records[1].front() == 'I');
    GAMENET_TEST_ASSERT(records[2].front() == 'W');
    GAMENET_TEST_ASSERT(records[3].front() == 'E');

    reset();
    gamenet::base::Logger::setLogLevel(gamenet::base::Logger::INFO);
    int evaluated = 0;
    DLOG << ++evaluated;
    DLOG_IF(true) << ++evaluated;
    LOG_IF(false) << ++evaluated;
    LOG << "kept";
    GAMENET_TEST_ASSERT(evaluated == 0);
    GAMENET_TEST_ASSERT(records.size() == 1);
    GAMENET_TEST_ASSERT(contains(records.back(), "kept"));

    reset();
    for (int i = 0; i < 5; ++i) {
        LOG_EVERY_N(2) << "every";
    }
    for (int i = 0; i < 5; ++i) {
        LOG_FIRST_N(3) << "first";
    }
    GAMENET_TEST_ASSERT(records.size() == 6);

    reset();
    TLOG("net") << "accepted";
    GAMENET_TEST_ASSERT(topics.size() == 1);
    GAMENET_TEST_ASSERT(topics.back() == "net");
    GAMENET_TEST_ASSERT(contains(records.back(), "[topic:net] accepted"));

    gamenet::base::Logger::resetForTesting();
    return 0;
}
```

- [ ] **Step 2: Register and run the unit test to verify it fails**

Run:

```bash
cmake --build build --target unit_base_test_logger
ctest --test-dir build --output-on-failure -R unit.base.test_logger
```

Expected: build fails because `Logger::resetForTesting`, `setTopicOutputFunction`, `DLOG`, `LOG`, `WLOG`, `ELOG`, `LOG_EVERY_N`, `LOG_FIRST_N`, and `TLOG` are not defined yet.

- [ ] **Step 3: Implement the minimal logger API**

Add declarations and macros in `include/gamenet/core/base/Logger.h`. Implement thread-safe callback state, coost-style header formatting, topic output, and reset support in `src/core/base/Logger.cc`.

- [ ] **Step 4: Run the unit test to verify it passes**

Run:

```bash
cmake --build build --target unit_base_test_logger
ctest --test-dir build --output-on-failure -R unit.base.test_logger
```

Expected: the new unit test passes.

## Task 2: Contract Tests For Fatal And System Logging

**Files:**
- Create: `tests/contract/base/test_logger_contract.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing contract test**

```cpp
#include "gamenet/core/base/Logger.h"
#include "support/TestAssert.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

int runChild(const char* mode, const char* outputPath) {
#ifdef _WIN32
    char exePath[MAX_PATH];
    DWORD size = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    GAMENET_TEST_ASSERT(size > 0);

    std::string command = "\"";
    command += exePath;
    command += "\" ";
    command += mode;
    command += " \"";
    command += outputPath;
    command += "\"";

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
    GAMENET_TEST_ASSERT(ok);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    pid_t pid = fork();
    GAMENET_TEST_ASSERT(pid >= 0);
    if (pid == 0) {
        execl("/proc/self/exe", "/proc/self/exe", mode, outputPath, nullptr);
        _exit(127);
    }
    int status = 0;
    GAMENET_TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
#endif
}

std::string readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "check") {
        gamenet::base::Logger::setOutputFunction([path = std::string(argv[2])](const char* msg, int len) {
            std::ofstream out(path, std::ios::binary | std::ios::app);
            out.write(msg, len);
        });
        CHECK_EQ(1, 2);
        return 0;
    }

    if (argc == 3 && std::string_view(argv[1]) == "syserr") {
        gamenet::base::Logger::setOutputFunction([path = std::string(argv[2])](const char* msg, int len) {
            std::ofstream out(path, std::ios::binary | std::ios::app);
            out.write(msg, len);
        });
        errno = EINVAL;
        LOG_SYSERR << "open failed";
        return 0;
    }

    const char* checkPath = "logger_check_contract.out";
    const int checkCode = runChild("check", checkPath);
    GAMENET_TEST_ASSERT(checkCode != 0);
    const std::string checkOutput = readFile(checkPath);
    GAMENET_TEST_ASSERT(contains(checkOutput, "check failed: 1 == 2"));
    GAMENET_TEST_ASSERT(contains(checkOutput, "1 vs 2"));

    const char* syserrPath = "logger_syserr_contract.out";
    const int syserrCode = runChild("syserr", syserrPath);
    GAMENET_TEST_ASSERT(syserrCode == 0);
    const std::string syserrOutput = readFile(syserrPath);
    GAMENET_TEST_ASSERT(contains(syserrOutput, "open failed"));
    GAMENET_TEST_ASSERT(contains(syserrOutput, "Invalid") || contains(syserrOutput, "invalid"));

    return 0;
}
```

- [ ] **Step 2: Run the contract test to verify it fails**

Run:

```bash
cmake --build build --target contract_base_test_logger_contract
ctest --test-dir build --output-on-failure -R contract.base.test_logger_contract
```

Expected: build fails because `CHECK_EQ` is not defined and `LOG_SYSERR` does not append system error text yet.

- [ ] **Step 3: Implement fatal checks and system-error records**

Add `CHECK*` macros that build a fatal logger record only on failure. Add a system-error constructor mode so `LOG_SYSERR` and `LOG_SYSFATAL` append `": <error message>"` before the final newline and fatal abort.

- [ ] **Step 4: Run the contract test to verify it passes**

Run:

```bash
cmake --build build --target contract_base_test_logger_contract
ctest --test-dir build --output-on-failure -R contract.base.test_logger_contract
```

Expected: the child process exits non-zero for failed `CHECK_EQ`; `LOG_SYSERR` exits zero and writes errno context.

## Task 3: Full Verification

**Files:**
- Existing build files and tests.

- [ ] **Step 1: Configure**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGAMENET_BUILD_TESTING=ON
```

Expected: configure completes successfully.

- [ ] **Step 2: Build**

Run:

```bash
cmake --build build --parallel
```

Expected: build completes successfully.

- [ ] **Step 3: Run the complete CTest suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all configured tests pass.
