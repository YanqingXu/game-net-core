#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace gamenet::net {

class EventLoop;

// A copyable, non-owning scheduling capability for one EventLoop. The handle
// never extends EventLoop lifetime and rejects work after loop admission closes.
class EventLoopExecutor {
public:
    using Functor = std::function<void()>;

    EventLoopExecutor() = default;

    bool tryQueue(Functor callback) const;
    bool available() const noexcept;
    // True while the caller is the owner and the loop is either accepting new
    // work or draining work that was accepted before admission closed.
    bool isInOwnerThread() const noexcept;
    std::uint64_t id() const noexcept;

private:
    struct State;

    explicit EventLoopExecutor(const std::shared_ptr<State>& state) noexcept;

    std::weak_ptr<State> state_;
    std::uint64_t id_{};

    friend class EventLoop;
};

}  // namespace gamenet::net
