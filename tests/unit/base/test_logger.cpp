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
