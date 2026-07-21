#pragma once

#include <cstddef>

namespace gamenet::net {

class Channel;
class EventLoop;

namespace detail {

// Loop-owned hysteresis controller for one TcpConnection. It owns no socket,
// Channel, or connection lifetime; it only applies read-interest transitions.
class ConnectionBackpressureController {
public:
    explicit ConnectionBackpressureController(EventLoop* loop);

    static void validateThresholds(
        std::size_t highWaterMark,
        std::size_t lowWaterMark);

    void configure(
        std::size_t highWaterMark,
        std::size_t lowWaterMark,
        std::size_t bufferedBytes,
        Channel& channel);
    void onConnectionEstablished(std::size_t bufferedBytes, Channel& channel);
    void onBufferedBytesChanged(std::size_t bufferedBytes, Channel& channel);
    void onClosed();

    bool readingEnabled() const noexcept;

private:
    void apply(std::size_t bufferedBytes, Channel& channel);

    EventLoop* loop_;
    std::size_t highWaterMark_{0};
    std::size_t lowWaterMark_{0};
    bool active_{false};
    bool readingEnabled_{true};
};

}  // namespace detail
}  // namespace gamenet::net
