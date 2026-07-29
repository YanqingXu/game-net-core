#include "gamenet/core/net/DeadlineQueue.h"

#include "gamenet/core/net/EventLoop.h"

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace gamenet::net {

void DeadlineQueueOptions::validate() const {
    if (resolution <= std::chrono::steady_clock::duration::zero()) {
        throw std::invalid_argument(
            "DeadlineQueue resolution must be positive");
    }
    if (maxExpiredPerAdvance == 0) {
        throw std::invalid_argument(
            "DeadlineQueue expiration budget must be positive");
    }
}

struct DeadlineQueue::Impl {
    struct Entry {
        DeadlineToken token;
        gamenet::base::Timestamp deadline;
    };

    using Bucket = std::list<Entry>;
    using BucketMap = std::map<gamenet::base::Timestamp, Bucket>;

    struct Location {
        gamenet::base::Timestamp bucketDeadline;
        Bucket::iterator entry;
    };

    Impl(EventLoop* owner, DeadlineQueueOptions queueOptions)
        : ownerLoop(owner), options(queueOptions) {
        if (!ownerLoop) {
            throw std::invalid_argument(
                "DeadlineQueue requires an owner EventLoop");
        }
        options.validate();
        ownerLoop->assertInLoopThread();
    }

    void assertOwner() const {
        ownerLoop->assertInLoopThread();
    }

    gamenet::base::Timestamp quantize(
        gamenet::base::Timestamp deadline) const {
        const auto raw = deadline.time_since_epoch();
        const auto quotient = raw / options.resolution;
        auto rounded = options.resolution * quotient;
        if (rounded < raw) {
            const auto maximum =
                gamenet::base::Timestamp::max().time_since_epoch();
            if (maximum - rounded < options.resolution) {
                return gamenet::base::Timestamp::max();
            }
            rounded += options.resolution;
        }
        return gamenet::base::Timestamp(rounded);
    }

    void eraseLocation(
        std::unordered_map<DeadlineKey, Location>::iterator location) {
        const auto bucket = buckets.find(location->second.bucketDeadline);
        if (bucket == buckets.end()) {
            throw std::logic_error(
                "DeadlineQueue index references a missing bucket");
        }
        bucket->second.erase(location->second.entry);
        entries.erase(location);
        if (bucket->second.empty()) {
            buckets.erase(bucket);
        }
    }

    EventLoop* ownerLoop;
    DeadlineQueueOptions options;
    std::uint64_t nextGeneration{1};
    BucketMap buckets;
    std::unordered_map<DeadlineKey, Location> entries;
};

DeadlineQueue::DeadlineQueue(
    EventLoop* ownerLoop,
    DeadlineQueueOptions options)
    : impl_(std::make_unique<Impl>(ownerLoop, options)) {}

DeadlineQueue::~DeadlineQueue() {
    impl_->assertOwner();
}

DeadlineToken DeadlineQueue::schedule(
    DeadlineKey key,
    gamenet::base::Timestamp deadline) {
    impl_->assertOwner();
    if (impl_->nextGeneration ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "DeadlineQueue generation space exhausted");
    }

    const DeadlineToken token{key, impl_->nextGeneration};
    const auto bucketDeadline = impl_->quantize(deadline);
    auto [bucket, insertedBucket] =
        impl_->buckets.try_emplace(bucketDeadline);
    try {
        bucket->second.push_back(Impl::Entry{token, deadline});
    } catch (...) {
        if (insertedBucket && bucket->second.empty()) {
            impl_->buckets.erase(bucket);
        }
        throw;
    }
    const auto newEntry = std::prev(bucket->second.end());

    const auto existing = impl_->entries.find(key);
    if (existing != impl_->entries.end()) {
        const auto oldBucketDeadline = existing->second.bucketDeadline;
        const auto oldEntry = existing->second.entry;
        existing->second = Impl::Location{bucketDeadline, newEntry};

        const auto oldBucket = impl_->buckets.find(oldBucketDeadline);
        if (oldBucket == impl_->buckets.end()) {
            throw std::logic_error(
                "DeadlineQueue replacement references a missing bucket");
        }
        oldBucket->second.erase(oldEntry);
        if (oldBucket->second.empty()) {
            impl_->buckets.erase(oldBucket);
        }
    } else {
        try {
            impl_->entries.emplace(
                key,
                Impl::Location{bucketDeadline, newEntry});
        } catch (...) {
            bucket->second.erase(newEntry);
            if (bucket->second.empty()) {
                impl_->buckets.erase(bucket);
            }
            throw;
        }
    }

    ++impl_->nextGeneration;
    return token;
}

bool DeadlineQueue::cancel(DeadlineToken token) {
    impl_->assertOwner();
    if (!token.valid()) {
        return false;
    }
    const auto location = impl_->entries.find(token.key);
    if (location == impl_->entries.end() ||
        location->second.entry->token != token) {
        return false;
    }
    impl_->eraseLocation(location);
    return true;
}

DeadlineAdvanceResult DeadlineQueue::advance(
    gamenet::base::Timestamp now) {
    impl_->assertOwner();

    DeadlineAdvanceResult result;
    if (impl_->buckets.empty() ||
        impl_->buckets.begin()->first > now) {
        return result;
    }

    result.oldestReadyLatency =
        now - impl_->buckets.begin()->first;
    result.expired.reserve(std::min(
        impl_->options.maxExpiredPerAdvance,
        impl_->entries.size()));

    while (result.expired.size() <
               impl_->options.maxExpiredPerAdvance &&
           !impl_->buckets.empty() &&
           impl_->buckets.begin()->first <= now) {
        auto bucket = impl_->buckets.begin();
        auto entry = bucket->second.begin();
        result.expired.push_back(
            DeadlineExpiration{entry->token, entry->deadline});

        const auto location = impl_->entries.find(entry->token.key);
        if (location == impl_->entries.end() ||
            location->second.entry->token != entry->token) {
            throw std::logic_error(
                "DeadlineQueue bucket/index generation mismatch");
        }
        impl_->entries.erase(location);
        bucket->second.erase(entry);
        if (bucket->second.empty()) {
            impl_->buckets.erase(bucket);
        }
    }

    result.readyRemaining =
        !impl_->buckets.empty() &&
        impl_->buckets.begin()->first <= now;
    return result;
}

void DeadlineQueue::clear() {
    impl_->assertOwner();
    impl_->entries.clear();
    impl_->buckets.clear();
}

std::size_t DeadlineQueue::size() const {
    impl_->assertOwner();
    return impl_->entries.size();
}

std::optional<gamenet::base::Timestamp>
DeadlineQueue::nextBucketDeadline() const {
    impl_->assertOwner();
    if (impl_->buckets.empty()) {
        return std::nullopt;
    }
    return impl_->buckets.begin()->first;
}

}  // namespace gamenet::net
