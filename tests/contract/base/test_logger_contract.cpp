#include "gamenet/core/base/Logger.h"
#include "support/TestAssert.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
