#include "gamenet/core/base/Logger.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace gamenet::base {

namespace {

std::atomic<int> g_logLevel{Logger::INFO};
std::mutex g_callbackMutex;
Logger::OutputFunc g_outputFunc;
Logger::TopicOutputFunc g_topicOutputFunc;
Logger::FlushFunc g_flushFunc;

void defaultOutput(const char* msg, int len) {
    std::fwrite(msg, 1, static_cast<std::size_t>(len), stderr);
}

void defaultFlush() {
    std::fflush(stderr);
}

struct CallbackSnapshot {
    Logger::OutputFunc output;
    Logger::TopicOutputFunc topicOutput;
    Logger::FlushFunc flush;
};

CallbackSnapshot callbacks() {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    return CallbackSnapshot{
        g_outputFunc,
        g_topicOutputFunc,
        g_flushFunc,
    };
}

std::string threadIdString() {
    std::ostringstream os;
    os << std::this_thread::get_id();
    return os.str();
}

std::string systemErrorMessage(int error) {
    return std::generic_category().message(error);
}

}  // namespace

char Logger::levelChar(LogLevel level) {
    static const char chars[NUM_LOG_LEVELS] = {
        'T', 'D', 'I', 'W', 'E', 'F',
    };
    return chars[level];
}

const char* Logger::levelName(LogLevel level) {
    static const char* names[NUM_LOG_LEVELS] = {
        "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL",
    };
    return names[level];
}

const char* Logger::extractFileName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* separator = slash > backslash ? slash : backslash;
    return separator ? separator + 1 : path;
}

Logger::Logger(const char* file, int line, LogLevel level)
    : file_(extractFileName(file)),
      line_(line),
      level_(level),
      includeSystemError_(false),
      savedSystemError_(0),
      topic_() {
    formatHeader();
}

Logger::Logger(const char* file, int line, LogLevel level, const char* topic)
    : file_(extractFileName(file)),
      line_(line),
      level_(level),
      includeSystemError_(false),
      savedSystemError_(0),
      topic_(topic ? topic : "") {
    formatHeader();
}

Logger::Logger(const char* file, int line, LogLevel level, int savedSystemError)
    : file_(extractFileName(file)),
      line_(line),
      level_(level),
      includeSystemError_(true),
      savedSystemError_(savedSystemError),
      topic_() {
    formatHeader();
}

Logger::~Logger() {
    if (includeSystemError_) {
        buffer_.append(": ");
        buffer_.append(systemErrorMessage(savedSystemError_));
    }
    buffer_ += '\n';

    const auto snapshot = callbacks();
    const int len = static_cast<int>(buffer_.size());
    if (!topic_.empty() && snapshot.topicOutput) {
        snapshot.topicOutput(topic_.c_str(), buffer_.data(), len);
    } else {
        const auto& output = snapshot.output ? snapshot.output : Logger::OutputFunc(defaultOutput);
        output(buffer_.data(), len);
    }

    if (level_ == FATAL) {
        const auto& flush = snapshot.flush ? snapshot.flush : Logger::FlushFunc(defaultFlush);
        flush();
        std::abort();
    }
}

void Logger::formatHeader() {
    // Format: "I0708 11:42:33.123 thread-id file.cc:42] "
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    struct tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &tt);
#else
    ::localtime_r(&tt, &tm);
#endif

    char timeBuf[32];
    const int timeLen = std::snprintf(timeBuf, sizeof(timeBuf),
        "%c%02d%02d %02d:%02d:%02d.%03ld",
        levelChar(level_), tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long>(ms.count()));

    buffer_.reserve(128);
    buffer_.append(timeBuf, static_cast<std::size_t>(timeLen));
    buffer_ += ' ';
    buffer_.append(threadIdString());
    buffer_ += ' ';
    buffer_.append(file_);
    buffer_ += ':';

    char lineBuf[16];
    const int lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%d", line_);
    buffer_.append(lineBuf, static_cast<std::size_t>(lineLen));
    buffer_.append("] ", 2);

    if (!topic_.empty()) {
        buffer_.append("[topic:", 7);
        buffer_.append(topic_);
        buffer_.append("] ", 2);
    }
}

Logger& Logger::operator<<(const char* str) {
    buffer_.append(str ? str : "(null)");
    return *this;
}

Logger& Logger::operator<<(const std::string& str) {
    buffer_.append(str);
    return *this;
}

Logger& Logger::operator<<(std::string_view str) {
    buffer_.append(str);
    return *this;
}

Logger& Logger::operator<<(int val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(unsigned int val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(long val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(unsigned long val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(long long val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(unsigned long long val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(double val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(char c) {
    buffer_ += c;
    return *this;
}

Logger::LogLevel Logger::logLevel() {
    return static_cast<LogLevel>(g_logLevel.load(std::memory_order_relaxed));
}

bool Logger::shouldLog(LogLevel level) {
    return static_cast<int>(level) >= g_logLevel.load(std::memory_order_relaxed);
}

void Logger::setLogLevel(LogLevel level) {
    g_logLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

void Logger::setOutputFunction(OutputFunc func) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_outputFunc = std::move(func);
}

void Logger::setTopicOutputFunction(TopicOutputFunc func) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_topicOutputFunc = std::move(func);
}

void Logger::setFlushFunction(FlushFunc func) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_flushFunc = std::move(func);
}

void Logger::resetForTesting() {
    g_logLevel.store(static_cast<int>(INFO), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_outputFunc = nullptr;
    g_topicOutputFunc = nullptr;
    g_flushFunc = nullptr;
}

Logger::LogLevel logLevel() {
    return Logger::logLevel();
}

}  // namespace gamenet::base
