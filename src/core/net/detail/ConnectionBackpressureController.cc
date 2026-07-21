#include "ConnectionBackpressureController.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"

#include <stdexcept>

namespace gamenet::net::detail {

ConnectionBackpressureController::ConnectionBackpressureController(EventLoop* loop)
    : loop_(loop) {
}

void ConnectionBackpressureController::validateThresholds(
    std::size_t highWaterMark,
    std::size_t lowWaterMark) {
    if (highWaterMark == 0) {
        throw std::invalid_argument("backpressure high water mark must be positive");
    }
    if (lowWaterMark >= highWaterMark) {
        throw std::invalid_argument(
            "backpressure low water mark must be smaller than high water mark");
    }
}

void ConnectionBackpressureController::configure(
    std::size_t highWaterMark,
    std::size_t lowWaterMark,
    std::size_t bufferedBytes,
    Channel& channel) {
    loop_->assertInLoopThread();
    validateThresholds(highWaterMark, lowWaterMark);
    highWaterMark_ = highWaterMark;
    lowWaterMark_ = lowWaterMark;
    apply(bufferedBytes, channel);
}

void ConnectionBackpressureController::onConnectionEstablished(
    std::size_t bufferedBytes,
    Channel& channel) {
    loop_->assertInLoopThread();
    active_ = true;
    readingEnabled_ = true;
    apply(bufferedBytes, channel);
}

void ConnectionBackpressureController::onBufferedBytesChanged(
    std::size_t bufferedBytes,
    Channel& channel) {
    loop_->assertInLoopThread();
    apply(bufferedBytes, channel);
}

void ConnectionBackpressureController::onClosed() {
    loop_->assertInLoopThread();
    active_ = false;
    readingEnabled_ = false;
}

bool ConnectionBackpressureController::readingEnabled() const noexcept {
    return readingEnabled_;
}

void ConnectionBackpressureController::apply(
    std::size_t bufferedBytes,
    Channel& channel) {
    loop_->assertInLoopThread();
    if (!active_) {
        return;
    }

    if (readingEnabled_ && bufferedBytes >= highWaterMark_) {
        readingEnabled_ = false;
        if (channel.isReading()) {
            channel.disableReading();
        }
        return;
    }

    if (!readingEnabled_ && bufferedBytes <= lowWaterMark_) {
        readingEnabled_ = true;
        if (!channel.isReading()) {
            channel.enableReading();
        }
    }
}

}  // namespace gamenet::net::detail
