#include "gamenet/core/base/Logger.h"

#include "support/TestAssert.h"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <thread>
#include <vector>

int main() {
    constexpr int producerCount = 4;
    constexpr int recordsPerProducer = 256;
    constexpr int expectedRecords = producerCount * recordsPerProducer;

    std::atomic<int> outputACount{0};
    std::atomic<int> outputBCount{0};
    std::atomic<int> invalidRecords{0};
    std::atomic<int> producersDone{0};
    std::atomic<bool> start{false};

    auto validateRecord = [&invalidRecords](const char* msg, int len) {
        if (msg == nullptr || len <= 0) {
            invalidRecords.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::string_view record(msg, static_cast<std::size_t>(len));
        if (record.front() != 'I' || record.find("logger-thread-contract") == std::string_view::npos) {
            invalidRecords.fetch_add(1, std::memory_order_relaxed);
        }
    };

    gamenet::base::Logger::OutputFunc outputA = [&](const char* msg, int len) {
        validateRecord(msg, len);
        outputACount.fetch_add(1, std::memory_order_relaxed);
    };
    gamenet::base::Logger::OutputFunc outputB = [&](const char* msg, int len) {
        validateRecord(msg, len);
        outputBCount.fetch_add(1, std::memory_order_relaxed);
    };
    gamenet::base::Logger::TopicOutputFunc topicOutput = [](const char*, const char*, int) {};
    gamenet::base::Logger::FlushFunc flush = [] {};

    gamenet::base::Logger::resetForTesting();
    gamenet::base::Logger::setLogLevel(gamenet::base::Logger::INFO);
    gamenet::base::Logger::setOutputFunction(outputA);

    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int record = 0; record < recordsPerProducer; ++record) {
                LOG_INFO << "logger-thread-contract " << producer << ' ' << record;
            }
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }

    std::thread reconfigurer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        int iteration = 0;
        while (iteration < 512 || producersDone.load(std::memory_order_acquire) != producerCount) {
            gamenet::base::Logger::setLogLevel(
                iteration % 2 == 0 ? gamenet::base::Logger::TRACE : gamenet::base::Logger::INFO);
            gamenet::base::Logger::setOutputFunction(iteration % 2 == 0 ? outputA : outputB);
            gamenet::base::Logger::setTopicOutputFunction(
                iteration % 2 == 0 ? topicOutput : gamenet::base::Logger::TopicOutputFunc{});
            gamenet::base::Logger::setFlushFunction(
                iteration % 2 == 0 ? flush : gamenet::base::Logger::FlushFunc{});
            ++iteration;
            std::this_thread::yield();
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }
    reconfigurer.join();

    GAMENET_TEST_ASSERT(invalidRecords.load(std::memory_order_relaxed) == 0);
    GAMENET_TEST_ASSERT(
        outputACount.load(std::memory_order_relaxed) + outputBCount.load(std::memory_order_relaxed) ==
        expectedRecords);

    std::atomic<int> reentrantCallbackCount{0};
    gamenet::base::Logger::setLogLevel(gamenet::base::Logger::INFO);
    gamenet::base::Logger::setOutputFunction([&](const char*, int) {
        reentrantCallbackCount.fetch_add(1, std::memory_order_relaxed);
        gamenet::base::Logger::setOutputFunction([&](const char*, int) {
            reentrantCallbackCount.fetch_add(1, std::memory_order_relaxed);
        });
    });

    LOG_INFO << "logger-thread-contract reentrant-config-first";
    LOG_INFO << "logger-thread-contract reentrant-config-second";
    GAMENET_TEST_ASSERT(reentrantCallbackCount.load(std::memory_order_relaxed) == 2);

    gamenet::base::Logger::resetForTesting();
    return 0;
}
