#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/DeadlineQueue.h"
#include "gamenet/core/net/EventLoop.h"

#include "support/TestAssert.h"

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    const auto origin = gamenet::base::Timestamp{} + 1s;

    {
        gamenet::net::DeadlineQueue queue(
            &loop,
            {
                .resolution = 10ms,
                .maxExpiredPerAdvance = 2,
            });

        const auto first = queue.schedule(1, origin + 15ms);
        const auto second = queue.schedule(2, origin + 11ms);
        const auto third = queue.schedule(3, origin + 15ms);
        GAMENET_TEST_ASSERT(first.valid());
        GAMENET_TEST_ASSERT(second.valid());
        GAMENET_TEST_ASSERT(third.valid());
        GAMENET_TEST_ASSERT(queue.size() == 3);
        GAMENET_TEST_ASSERT(queue.nextBucketDeadline() == origin + 20ms);

        // deadline-queue-no-early-contract: quantization rounds upward.
        GAMENET_TEST_ASSERT(queue.advance(origin + 19ms).expired.empty());

        // deadline-queue-budget-contract: one bucket larger than the advance
        // budget retains its remainder for an immediate owner-loop continuation.
        auto firstBatch = queue.advance(origin + 20ms);
        GAMENET_TEST_ASSERT(firstBatch.expired.size() == 2);
        GAMENET_TEST_ASSERT(firstBatch.readyRemaining);
        GAMENET_TEST_ASSERT(firstBatch.expired[0].token == first);
        GAMENET_TEST_ASSERT(firstBatch.expired[1].token == second);

        auto secondBatch = queue.advance(origin + 20ms);
        GAMENET_TEST_ASSERT(secondBatch.expired.size() == 1);
        GAMENET_TEST_ASSERT(secondBatch.expired[0].token == third);
        GAMENET_TEST_ASSERT(!secondBatch.readyRemaining);
        GAMENET_TEST_ASSERT(queue.size() == 0);
    }

    {
        gamenet::net::DeadlineQueue queue(&loop);
        const auto zeroKey = queue.schedule(0, origin + 1ms);
        GAMENET_TEST_ASSERT(zeroKey.valid());
        GAMENET_TEST_ASSERT(
            queue.advance(origin + 20ms).expired.front().token == zeroKey);
    }

    {
        gamenet::net::DeadlineQueue queue(
            &loop,
            {
                .resolution = 1ms,
                .maxExpiredPerAdvance = 4,
            });
        const auto stale = queue.schedule(7, origin + 5ms);
        const auto replacement = queue.schedule(7, origin + 30ms);
        GAMENET_TEST_ASSERT(replacement.generation != stale.generation);
        // deadline-queue-generation-contract: stale cancellation cannot remove
        // a same-key replacement.
        GAMENET_TEST_ASSERT(!queue.cancel(stale));
        GAMENET_TEST_ASSERT(queue.advance(origin + 5ms).expired.empty());
        GAMENET_TEST_ASSERT(queue.cancel(replacement));
        GAMENET_TEST_ASSERT(queue.size() == 0);
    }

    {
        gamenet::net::DeadlineQueue queue(
            &loop,
            {
                .resolution = 1ms,
                .maxExpiredPerAdvance = 8,
            });
        for (std::uint64_t key = 1; key <= 10000; ++key) {
            (void)queue.schedule(key, origin + 1h);
        }
        (void)queue.schedule(10001, origin + 1ms);
        // deadline-queue-future-isolation-contract: advancing one ready bucket
        // leaves the large future population indexed without scanning it out.
        const auto ready = queue.advance(origin + 1ms);
        GAMENET_TEST_ASSERT(ready.expired.size() == 1);
        GAMENET_TEST_ASSERT(ready.expired.front().token.key == 10001);
        GAMENET_TEST_ASSERT(queue.size() == 10000);
        queue.clear();
        GAMENET_TEST_ASSERT(queue.size() == 0);
        GAMENET_TEST_ASSERT(!queue.nextBucketDeadline().has_value());
    }

    {
        bool invalidOptionsRejected = false;
        try {
            gamenet::net::DeadlineQueue invalid(
                &loop,
                {
                    .resolution = 0ms,
                    .maxExpiredPerAdvance = 0,
                });
        } catch (const std::invalid_argument&) {
            invalidOptionsRejected = true;
        }
        GAMENET_TEST_ASSERT(invalidOptionsRejected);
    }

    {
        gamenet::net::DeadlineQueue queue(&loop);
        bool wrongThreadRejected = false;
        std::thread worker([&] {
            try {
                (void)queue.schedule(1, origin);
            } catch (const std::runtime_error&) {
                wrongThreadRejected = true;
            }
        });
        worker.join();
        GAMENET_TEST_ASSERT(wrongThreadRejected);
    }

    return 0;
}
