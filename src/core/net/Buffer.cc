#include "gamenet/core/net/Buffer.h"

#include "gamenet/core/net/SocketsOps.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <sys/uio.h>
#endif

namespace gamenet::net {

void BufferRetentionOptions::validate() const {
    if (maxRetainedCapacityBytes < Buffer::kCheapPrepend +
                                       Buffer::kInitialSize) {
        throw std::invalid_argument(
            "Buffer retained capacity must hold the initial storage");
    }
    if (trimThresholdBytes >
        maxRetainedCapacityBytes - Buffer::kCheapPrepend) {
        throw std::invalid_argument(
            "Buffer trim threshold must fit below retained capacity");
    }
}

Buffer::Buffer(BufferRetentionOptions retentionOptions)
    : retentionOptions_(retentionOptions),
      buffer_(kCheapPrepend + kInitialSize),
      readerIndex_(kCheapPrepend),
      writerIndex_(kCheapPrepend),
      peakRetainedCapacityBytes_(buffer_.capacity()) {
    retentionOptions_.validate();
}

std::size_t Buffer::readableBytes() const noexcept {
    return writerIndex_ - readerIndex_;
}

std::size_t Buffer::writableBytes() const noexcept {
    return buffer_.size() - writerIndex_;
}

std::size_t Buffer::prependableBytes() const noexcept {
    return readerIndex_;
}

BufferRetentionSnapshot Buffer::retentionSnapshot() const noexcept {
    return {
        .retainedCapacityBytes = buffer_.capacity(),
        .peakRetainedCapacityBytes = peakRetainedCapacityBytes_,
        .trimCount = trimCount_,
        .trimArmed = trimArmed_,
    };
}

std::size_t Buffer::maxRetainedCapacityBytes() const noexcept {
    return retentionOptions_.maxRetainedCapacityBytes;
}

std::size_t Buffer::trimThresholdBytes() const noexcept {
    return retentionOptions_.trimThresholdBytes;
}

bool Buffer::trimRetainedCapacity() noexcept {
    if (!trimArmed_ ||
        readableBytes() > retentionOptions_.trimThresholdBytes) {
        return false;
    }

    try {
        std::vector<char> trimmed(
            retentionOptions_.maxRetainedCapacityBytes);
        const std::size_t readable = readableBytes();
        if (readable != 0) {
            std::copy(
                begin() + readerIndex_,
                begin() + writerIndex_,
                trimmed.data() + kCheapPrepend);
        }
        buffer_.swap(trimmed);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
        ++trimCount_;
        trimArmed_ =
            buffer_.capacity() >
            retentionOptions_.maxRetainedCapacityBytes;
        return true;
    } catch (...) {
        return false;
    }
}

const char* Buffer::peek() const noexcept {
    return begin() + readerIndex_;
}

char* Buffer::beginWrite() noexcept {
    return begin() + writerIndex_;
}

const char* Buffer::beginWrite() const noexcept {
    return begin() + writerIndex_;
}

void Buffer::retrieve(std::size_t len) {
    if (len < readableBytes()) {
        readerIndex_ += len;
        (void)trimRetainedCapacity();
    } else {
        retrieveAll();
    }
}

void Buffer::retrieveUntil(const char* end) {
    retrieve(static_cast<std::size_t>(end - peek()));
}

void Buffer::retrieveAll() {
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;
    (void)trimRetainedCapacity();
}

std::string Buffer::retrieveAllAsString() {
    return retrieveAsString(readableBytes());
}

std::string Buffer::retrieveAsString(std::size_t len) {
    len = std::min(len, readableBytes());
    std::string result(peek(), len);
    retrieve(len);
    return result;
}

void Buffer::append(const char* data, std::size_t len) {
    ensureWritableBytes(len);
    std::memcpy(beginWrite(), data, len);
    hasWritten(len);
}

void Buffer::append(std::string_view data) {
    append(data.data(), data.size());
}

void Buffer::append(const std::string& data) {
    append(data.data(), data.size());
}

void Buffer::ensureWritableBytes(std::size_t len) {
    if (writableBytes() < len) {
        makeSpace(len);
    }
}

void Buffer::hasWritten(std::size_t len) {
    writerIndex_ += len;
}

ssize_t Buffer::readFd(SocketFd fd, int* savedErrno, std::size_t maxReadBytes) {
    if (maxReadBytes == 0) {
        return 0;
    }
#ifdef _WIN32
    const std::size_t readLimit = std::min<std::size_t>(65536, maxReadBytes);
    if (writableBytes() < readLimit) {
        makeSpace(readLimit);
    }
    const ssize_t n = sockets::read(fd, beginWrite(), readLimit);
    if (n < 0) {
        *savedErrno = sockets::lastError();
        return n;
    }
    writerIndex_ += static_cast<std::size_t>(n);
    return n;
#else
    char extraBuffer[65536];
    struct iovec vec[2];
    const std::size_t writable = std::min(writableBytes(), maxReadBytes);
    const std::size_t extra =
        std::min(sizeof(extraBuffer), maxReadBytes - writable);

    vec[0].iov_base = beginWrite();
    vec[0].iov_len = writable;
    vec[1].iov_base = extraBuffer;
    vec[1].iov_len = extra;

    const int iovcnt = extra > 0 ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = sockets::lastError();
        return n;
    }
    if (static_cast<std::size_t>(n) <= writable) {
        writerIndex_ += static_cast<std::size_t>(n);
    } else {
        writerIndex_ = buffer_.size();
        append(extraBuffer, static_cast<std::size_t>(n) - writable);
    }
    return n;
#endif
}

ssize_t Buffer::writeFd(SocketFd fd, int* savedErrno) {
    const ssize_t n = sockets::write(fd, peek(), readableBytes());
    if (n < 0) {
        *savedErrno = sockets::lastError();
    }
    return n;
}

char* Buffer::begin() noexcept {
    return buffer_.data();
}

const char* Buffer::begin() const noexcept {
    return buffer_.data();
}

void Buffer::makeSpace(std::size_t len) {
    if (writableBytes() + prependableBytes() - kCheapPrepend < len) {
        buffer_.resize(writerIndex_ + len);
        updateRetentionState();
        return;
    }

    const std::size_t readable = readableBytes();
    std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
    readerIndex_ = kCheapPrepend;
    writerIndex_ = readerIndex_ + readable;
}

void Buffer::updateRetentionState() noexcept {
    peakRetainedCapacityBytes_ =
        std::max(peakRetainedCapacityBytes_, buffer_.capacity());
    if (buffer_.capacity() >
        retentionOptions_.maxRetainedCapacityBytes) {
        trimArmed_ = true;
    }
}

}  // namespace gamenet::net
