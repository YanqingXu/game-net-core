// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/rpc/RpcChannel.h"
#include "gamenet/rpc/RpcTransport.h"
#include "gamenet/protocol/PacketFramer.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace gamenet::test {
// Synthetic transport controls admission only. All real channel/timer/continuation
// state stays on a real owner loop. No socket/network performance is modeled.
class RpcStateSequence {
    using Rpc = rpc::RpcChannel;
    struct Transport final : rpc::RpcTransport {
        net::EventLoop* loop;
        rpc::RpcTransportState phase{rpc::RpcTransportState::Ready};
        unsigned sendMode{};
        std::weak_ptr<Rpc> channel;
        explicit Transport(net::EventLoop& owner) : loop(&owner) {}
        net::EventLoop* ownerLoop() const noexcept override { return loop; }
        rpc::RpcTransportState state() const noexcept override { return phase; }
        bool trySend(std::string_view) override {
            if (sendMode == 2) throw std::runtime_error("injected send failure");
            if (sendMode == 3) { phase = rpc::RpcTransportState::Closed; if (auto c = channel.lock()) c->onDisconnect(); }
            return sendMode != 1;
        }
        void abort() override { phase = rpc::RpcTransportState::Closed; }
    };
    struct Call { bool admitted{}; unsigned completions{}; std::uint64_t id{}; std::size_t bytes{}, generation{}; };
    net::EventLoop loop_{{.maxPendingFunctors = 8, .reservedPendingFunctors = 0, .maxFunctorsPerIteration = 4}};
    rpc::RpcOptions options_;
    std::shared_ptr<Transport> transport_;
    std::shared_ptr<Rpc> channel_;
    std::array<Call, 512> calls_{};
    std::size_t count_{}, generation_{};
    std::uint64_t incoming_{};
    std::vector<Rpc::Reply> replies_;
    std::vector<std::weak_ptr<Rpc>> retired_;
    unsigned callbackMode_{};
    bool inCallback_{};

    void check() {
        const auto s = channel_->snapshot();
        std::size_t pending = 0, bytes = 0, accepted = 0, completed = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& call = calls_[i];
            GAMENET_TEST_ASSERT(call.completions <= (call.admitted ? 1u : 0u));
            if (call.generation != generation_ || !call.admitted) continue;
            ++accepted;
            if (call.completions) ++completed;
            else { ++pending; bytes += call.bytes; }
        }
        GAMENET_TEST_ASSERT(s.acceptedCalls == accepted && s.completedCalls == completed &&
            s.pendingRequests == pending && s.pendingBytes == bytes && accepted == completed + pending);
        GAMENET_TEST_ASSERT(pending <= options_.maxPendingRequests && bytes <= options_.maxPendingBytes &&
            s.inboundRequests <= options_.maxInboundRequests && s.inboundBytes <= options_.maxInboundBytes);
        if (s.closed) GAMENET_TEST_ASSERT(!pending && !bytes && !s.inboundRequests && !s.inboundBytes);
    }
    void replace() {
        if (channel_) {
            (void)channel_->close(); check(); retired_.push_back(channel_); channel_.reset();
            for (const auto& reply : replies_)
                GAMENET_TEST_ASSERT(reply({rpc::RpcStatus::Ok, "stale"}) == rpc::RpcOperation::Closed);
        }
        replies_.clear(); incoming_ = 0; ++generation_;
        transport_ = std::make_shared<Transport>(loop_);
        channel_ = Rpc::createWithTransport(transport_, [&](const auto& request, auto reply) {
            if (request.method == 1) {
                const auto sent = reply({rpc::RpcStatus::Ok, request.payload});
                GAMENET_TEST_ASSERT(sent == rpc::RpcOperation::Accepted || sent == rpc::RpcOperation::SendRejected);
                const auto duplicate = reply({rpc::RpcStatus::Ok, "duplicate"});
                GAMENET_TEST_ASSERT(duplicate == (channel_->snapshot().closed ? rpc::RpcOperation::Closed : rpc::RpcOperation::NotFound));
            } else if (request.method == 2) replies_.push_back(std::move(reply));
            else if (request.method == 3) throw std::runtime_error("injected handler failure");
            else (void)channel_->close();
        }, options_);
        transport_->channel = channel_;
    }
    void call(unsigned length) {
        GAMENET_TEST_ASSERT(count_ < calls_.size());
        const auto index = count_++;
        auto& record = calls_[index]; record.generation = generation_; record.bytes = 28 + length;
        const auto result = channel_->call(1, std::string(length, 'p'), std::chrono::seconds(60),
            [&, index](rpc::RpcResult value) {
                auto& entry = calls_[index];
                GAMENET_TEST_ASSERT(entry.admitted && ++entry.completions == 1 && loop_.isInLoopThread());
                if (value.status == rpc::RpcStatus::Ok || value.status == rpc::RpcStatus::RemoteError)
                    GAMENET_TEST_ASSERT(value.payload == "response");
                if (!inCallback_) {
                    inCallback_ = true;
                    if (callbackMode_ == 1) (void)channel_->close();
                    if (callbackMode_ == 2) for (std::size_t i = 0; i < count_; ++i)
                        if (calls_[i].generation == generation_ && calls_[i].admitted && !calls_[i].completions) {
                            (void)channel_->cancel(calls_[i].id); break;
                        }
                    inCallback_ = false;
                }
                if (callbackMode_ == 3) throw std::runtime_error("injected callback failure");
            });
        record.admitted = result.status == rpc::RpcOperation::Accepted; record.id = result.requestId;
        GAMENET_TEST_ASSERT(record.admitted ? result.requestId != 0 : result.requestId == 0);
    }
    void feed(std::string_view bytes) {
        net::Buffer input; input.append(bytes); channel_->onMessage(input);
        GAMENET_TEST_ASSERT(input.readableBytes() == 0);
    }
    static std::string wire(rpc::RpcMessage message) {
        protocol::PacketFramer framer; return *framer.encode(*rpc::RpcCodec::encode(message));
    }
public:
    explicit RpcStateSequence(unsigned budget) {
        options_.maxPayloadBytes = 32; options_.maxPendingRequests = 3; options_.maxPendingBytes = 96;
        options_.maxInboundRequests = 2; options_.maxInboundBytes = 64;
        options_.maxBufferedBytes = 256; options_.maxFramesPerDispatch = budget;
        replace();
    }
    void run(std::span<const std::uint8_t> bytes) {
        // At most 128 bounded actions, no wall-clock-dependent timeout actions.
        for (std::size_t offset = 0; offset + 3 < bytes.size() && offset < 512; offset += 4) {
            const unsigned op = bytes[offset] % 16, a = bytes[offset + 1], b = bytes[offset + 2];
            const auto id = count_ ? calls_[a % count_].id : 99;
            switch (op) {
            case 0: call(a % 40); break; // includes payload rejection
            case 1: (void)channel_->cancel(id); break;
            case 2: case 3: case 4: {
                const auto message = wire({op == 3 ? rpc::RpcMessageKind::Error : rpc::RpcMessageKind::Response,
                    id ? id : 99, 0, 0, "response"});
                feed(op == 4 ? message + message : message); break;
            }
            case 5: feed(wire({rpc::RpcMessageKind::Request, ++incoming_, a % 4 + 1, 60000, std::string(b % 8, 'i')})); break;
            case 6: if (!replies_.empty()) (void)replies_[a % replies_.size()]({rpc::RpcStatus::Ok, "reply"}); break;
            case 7: transport_->phase = a % 2 ? rpc::RpcTransportState::Ready : rpc::RpcTransportState::NotReady; break;
            case 8: transport_->sendMode = a % 4; break;
            case 9: (void)channel_->close(); break;
            case 10: channel_->onDisconnect(); channel_->onDisconnect(); break;
            case 11: { auto bad = wire({rpc::RpcMessageKind::Request, ++incoming_, 1, 60000, ""});
                bad[4 + a % 24] = static_cast<char>(b); feed(bad); break; }
            case 12: replace(); break;
            case 13: callbackMode_ = a % 4; break;
            case 14: { const auto frame = wire({rpc::RpcMessageKind::Response, id ? id : 99, 0, 0, "response"});
                const auto split = a % frame.size(); feed(std::string_view(frame).substr(0, split));
                feed(std::string_view(frame).substr(split)); break; }
            case 15: // Two frames force a continuation at budget=1; optionally fill the owner queue.
                if (a % 2) for (unsigned n = 0; n < 8; ++n) (void)loop_.executor().post([] {});
                feed(wire({rpc::RpcMessageKind::Response, id ? id : 99, 0, 0, "response"}) +
                     wire({rpc::RpcMessageKind::Response, id ? id : 99, 0, 0, "response"})); break;
            }
            check();
        }
        (void)channel_->close(); check(); retired_.push_back(channel_); channel_.reset();
        loop_.quit(); loop_.loop(); // retire accepted continuation leases and cancellation work
        for (const auto& weak : retired_) GAMENET_TEST_ASSERT(weak.expired());
        for (std::size_t i = 0; i < count_; ++i)
            GAMENET_TEST_ASSERT(calls_[i].completions == (calls_[i].admitted ? 1u : 0u));
        for (const auto& reply : replies_) GAMENET_TEST_ASSERT(reply({rpc::RpcStatus::Ok, "stale"}) == rpc::RpcOperation::Closed);
    }
};
inline void runRpcStateSequence(std::span<const std::uint8_t> bytes) {
    RpcStateSequence sequence(bytes.empty() || !(bytes[0] & 16) ? 8 : 1); sequence.run(bytes);
}
inline void verifyRpcStateSeeds() {
    // Explicit transitions ensure useful paths even before coverage-guided mutation.
    const std::vector<std::uint8_t> transitions{
        0,0,0,0, 0,1,0,0, 2,0,0,0, 4,0,0,0, 1,1,0,0,
        5,1,0,0, 6,0,0,0, 6,0,0,0, 7,0,0,0, 0,0,0,0,
        7,1,0,0, 8,1,0,0, 0,0,0,0, 8,0,0,0, 0,2,0,0,
        13,2,0,0, 0,0,0,0, 3,2,0,0, 10,0,0,0, 12,0,0,0,
        0,0,0,0, 8,3,0,0, 0,0,0,0, 12,0,0,0, 0,0,0,0, 11,0,88,0};
    runRpcStateSequence(transitions);
    for (std::uint32_t seed = 1; seed <= 32; ++seed) {
        std::array<std::uint8_t, 512> input{}; auto state = seed;
        for (auto& byte : input) { state = state * 1664525u + 1013904223u; byte = static_cast<std::uint8_t>(state >> 24); }
        runRpcStateSequence(input);
    }
}
} // namespace gamenet::test
