#pragma once

// Logger 提供同步、线程安全的基础日志能力。
// 它保留现有 LOG_xxx 宏，并补充 coost 风格的 DLOG/LOG/WLOG/ELOG/FLOG、
// TLOG(topic) 与 CHECK 宏。FATAL/CHECK 失败在输出后调用 std::abort()。
// Logger 不归任何 EventLoop 所有，也不修改 reactor 线程亲和状态。

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace gamenet::base {

class Logger {
public:
    enum LogLevel {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        kError,
        FATAL,
        NUM_LOG_LEVELS,
    };

    using OutputFunc = std::function<void(const char* msg, int len)>;
    using TopicOutputFunc = std::function<void(const char* topic, const char* msg, int len)>;
    using FlushFunc = std::function<void()>;

    Logger(const char* file, int line, LogLevel level);
    Logger(const char* file, int line, LogLevel level, const char* topic);
    Logger(const char* file, int line, LogLevel level, int savedSystemError);
    ~Logger();

    Logger& stream() { return *this; }

    Logger& operator<<(const char* str);
    Logger& operator<<(const std::string& str);
    Logger& operator<<(std::string_view str);
    Logger& operator<<(int val);
    Logger& operator<<(unsigned int val);
    Logger& operator<<(long val);
    Logger& operator<<(unsigned long val);
    Logger& operator<<(long long val);
    Logger& operator<<(unsigned long long val);
    Logger& operator<<(double val);
    Logger& operator<<(char c);

    template <typename T>
    Logger& operator<<(const T& val) {
        std::ostringstream os;
        os << val;
        buffer_ += os.str();
        return *this;
    }

    // Process-global runtime configuration and emission may be called from any thread.
    // Each record snapshots the callback set when its destructor emits. Replacing a
    // callback affects later snapshots; an in-flight snapshot may retain the old copy.
    // Callbacks run on the emitting thread without the configuration lock, may be
    // invoked concurrently, and may re-enter configuration. Callback-owned sinks,
    // captured mutable state, and recursive logging require caller synchronization.
    static LogLevel logLevel();
    static bool shouldLog(LogLevel level);
    static void setLogLevel(LogLevel level);
    static void setOutputFunction(OutputFunc func);
    static void setTopicOutputFunction(TopicOutputFunc func);
    static void setFlushFunction(FlushFunc func);

    // Test-only reset; call only at a quiescent test boundary with no concurrent
    // emission or configuration.
    static void resetForTesting();

private:
    void formatHeader();

    static char levelChar(LogLevel level);
    static const char* levelName(LogLevel level);
    static const char* extractFileName(const char* path);

    const char* file_;
    int line_;
    LogLevel level_;
    bool includeSystemError_;
    int savedSystemError_;
    std::string topic_;
    std::string buffer_;
};

// Global log level (for macro fast-path check)
Logger::LogLevel logLevel();

}  // namespace gamenet::base

// Convenience macros. Suppressed level/conditional logs do not construct a
// Logger object, so streamed expressions are not evaluated.

#define LOG_TRACE                                                          \
    if (::gamenet::base::Logger::shouldLog(::gamenet::base::Logger::TRACE)) \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::TRACE).stream()

#define LOG_DEBUG                                                          \
    if (::gamenet::base::Logger::shouldLog(::gamenet::base::Logger::DEBUG)) \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::DEBUG).stream()

#define LOG_INFO                                                           \
    if (::gamenet::base::Logger::shouldLog(::gamenet::base::Logger::INFO))  \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::INFO).stream()

#define LOG_WARN                                                           \
    if (::gamenet::base::Logger::shouldLog(::gamenet::base::Logger::WARN))  \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::WARN).stream()

#define LOG_ERROR                                                          \
    if (::gamenet::base::Logger::shouldLog(::gamenet::base::Logger::kError)) \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::kError).stream()

#define LOG_FATAL                                                          \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::FATAL).stream()

#define LOG_SYSERR                                                         \
    if (::gamenet::base::Logger::shouldLog(::gamenet::base::Logger::kError)) \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::kError, errno).stream()

#define LOG_SYSFATAL                                                       \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::FATAL, errno).stream()

#define DLOG LOG_DEBUG
#define LOG LOG_INFO
#define WLOG LOG_WARN
#define ELOG LOG_ERROR
#define FLOG LOG_FATAL << "fatal error! "

#define DLOG_IF(cond) if (cond) DLOG
#define LOG_IF(cond) if (cond) LOG
#define WLOG_IF(cond) if (cond) WLOG
#define ELOG_IF(cond) if (cond) ELOG
#define FLOG_IF(cond) if (cond) FLOG

#define TLOG(topic)                                                        \
    if (::gamenet::base::Logger::shouldLog(::gamenet::base::Logger::INFO)) \
    ::gamenet::base::Logger(__FILE__, __LINE__, ::gamenet::base::Logger::INFO, topic).stream()

#define TLOG_IF(topic, cond) if (cond) TLOG(topic)

#define GAMENET_LOG_CONCAT_IMPL(a, b) a##b
#define GAMENET_LOG_CONCAT(a, b) GAMENET_LOG_CONCAT_IMPL(a, b)

#define GAMENET_LOG_EVERY_N(n, what)                                      \
    static std::atomic<unsigned int> GAMENET_LOG_CONCAT(gamenet_log_counter_, __LINE__){0}; \
    if ((GAMENET_LOG_CONCAT(gamenet_log_counter_, __LINE__).fetch_add(1U, std::memory_order_relaxed) % static_cast<unsigned int>(n)) == 0U) what

#define GAMENET_LOG_FIRST_N(n, what)                                      \
    static std::atomic<unsigned int> GAMENET_LOG_CONCAT(gamenet_log_counter_, __LINE__){0}; \
    if (GAMENET_LOG_CONCAT(gamenet_log_counter_, __LINE__).load(std::memory_order_relaxed) < static_cast<unsigned int>(n) && \
        GAMENET_LOG_CONCAT(gamenet_log_counter_, __LINE__).fetch_add(1U, std::memory_order_relaxed) < static_cast<unsigned int>(n)) what

#define DLOG_EVERY_N(n) GAMENET_LOG_EVERY_N(n, DLOG)
#define LOG_EVERY_N(n) GAMENET_LOG_EVERY_N(n, LOG)
#define WLOG_EVERY_N(n) GAMENET_LOG_EVERY_N(n, WLOG)
#define ELOG_EVERY_N(n) GAMENET_LOG_EVERY_N(n, ELOG)

#define DLOG_FIRST_N(n) GAMENET_LOG_FIRST_N(n, DLOG)
#define LOG_FIRST_N(n) GAMENET_LOG_FIRST_N(n, LOG)
#define WLOG_FIRST_N(n) GAMENET_LOG_FIRST_N(n, WLOG)
#define ELOG_FIRST_N(n) GAMENET_LOG_FIRST_N(n, ELOG)

#define CHECK(cond)                                                        \
    if (!(cond)) LOG_FATAL << "check failed: " #cond "! "

#define CHECK_NOTNULL(p)                                                   \
    if ((p) == nullptr) LOG_FATAL << "check failed: " #p " mustn't be NULL! "

#define GAMENET_CHECK_OP(a, b, op)                                         \
    for (auto GAMENET_LOG_CONCAT(gamenet_check_values_, __LINE__) = std::make_pair((a), (b)); \
         !(GAMENET_LOG_CONCAT(gamenet_check_values_, __LINE__).first op GAMENET_LOG_CONCAT(gamenet_check_values_, __LINE__).second);) \
    LOG_FATAL << "check failed: " #a " " #op " " #b ", "           \
              << GAMENET_LOG_CONCAT(gamenet_check_values_, __LINE__).first \
              << " vs "                                                   \
              << GAMENET_LOG_CONCAT(gamenet_check_values_, __LINE__).second \
              << "! "

#define CHECK_EQ(a, b) GAMENET_CHECK_OP(a, b, ==)
#define CHECK_NE(a, b) GAMENET_CHECK_OP(a, b, !=)
#define CHECK_GE(a, b) GAMENET_CHECK_OP(a, b, >=)
#define CHECK_LE(a, b) GAMENET_CHECK_OP(a, b, <=)
#define CHECK_GT(a, b) GAMENET_CHECK_OP(a, b, >)
#define CHECK_LT(a, b) GAMENET_CHECK_OP(a, b, <)
