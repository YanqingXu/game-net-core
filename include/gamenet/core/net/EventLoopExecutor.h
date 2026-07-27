#pragma once

#include "gamenet/core/net/PostResult.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace gamenet::net {

class EventLoop;

// A copyable, non-owning scheduling capability for one EventLoop. The handle
// never extends EventLoop lifetime and rejects work after loop admission closes.
class EventLoopExecutor {
public:
    using Functor = std::function<void()>;

    EventLoopExecutor() = default;

    template <typename Function>
    PostResult post(Function&& callback) const noexcept {
        try {
            return postFunctor(Functor(std::forward<Function>(callback)));
        } catch (...) {
            return PostResult::QueueFull;
        }
    }

    bool tryQueue(Functor callback) const;
    bool available() const noexcept;
    // True while the caller is the owner and the loop is either accepting new
    // work or draining work that was accepted before admission closed.
    bool isInOwnerThread() const noexcept;
    std::uint64_t id() const noexcept;

private:
    struct State;

    PostResult postFunctor(Functor callback) const noexcept;
    explicit EventLoopExecutor(const std::shared_ptr<State>& state) noexcept;

    std::weak_ptr<State> state_;
    std::uint64_t id_{};

    friend class EventLoop;
};

}  // namespace gamenet::net
