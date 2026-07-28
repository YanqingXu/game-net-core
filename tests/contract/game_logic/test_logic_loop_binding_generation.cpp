#include "gamenet/game_logic/LogicLoop.h"
#include "gamenet/game_session/SessionManager.h"

#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"

#include <chrono>
#include <memory>
#include <string_view>

namespace {

class BindingEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    BindingEndpoint(std::uint64_t id, gamenet::net::EventLoop* loop)
        : id_{id}, loop_(loop) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return loop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view) override {
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_ = false;
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::DispatchResult requestClose(
        gamenet::transport::CloseReason reason) noexcept override {
        (void)close(reason);
        return gamenet::DispatchResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_; }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* loop_;
    bool open_{true};
};

gamenet::game_logic::GameCommand commandFor(
    const gamenet::game_session::SessionBinding& binding,
    std::string payload) {
    return {
        .sessionId = binding.sessionId(),
        .transportId = binding.transportId(),
        .binding = binding,
        .payload = std::move(payload),
    };
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    {
        gamenet::net::EventLoop loop;
        gamenet::game_session::SessionManager sessions(&loop);
        auto firstEndpoint = std::make_shared<BindingEndpoint>(1, &loop);
        auto replacementEndpoint = std::make_shared<BindingEndpoint>(2, &loop);
        const auto first = sessions.authenticate("player", firstEndpoint);
        GAMENET_TEST_ASSERT(first.session);
        const auto staleBinding = first.session->binding();

        int handlerCalls = 0;
        int outputCalls = 0;
        gamenet::game_logic::LogicLoop logic(
            &loop,
            {.tickInterval = 1ms, .maxCommandsPerTick = 8});
        logic.setHandler([&](gamenet::game_logic::GameCommand command) {
            ++handlerCalls;
            return std::optional{std::move(command)};
        });
        logic.setOutputCallback(
            [&](gamenet::game_logic::GameCommand) { ++outputCalls; });
        logic.start();
        GAMENET_TEST_ASSERT(
            logic.submit(commandFor(staleBinding, "queued-before-rebind")) ==
            gamenet::game_logic::SubmitResult::Accepted);

        const auto rebound = sessions.authenticate("player", replacementEndpoint);
        GAMENET_TEST_ASSERT(
            rebound.status == gamenet::game_session::AuthenticateStatus::Rebound);
        GAMENET_TEST_ASSERT(rebound.session->binding().generation() > staleBinding.generation());
        GAMENET_TEST_ASSERT(!staleBinding.isCurrent());

        loop.runAfter(5ms, [&] {
            GAMENET_TEST_ASSERT(handlerCalls == 0);
            GAMENET_TEST_ASSERT(outputCalls == 0);
            GAMENET_TEST_ASSERT(logic.queueSnapshot().droppedStale == 1);
            (void)logic.stop();
            loop.quit();
        });
        loop.loop();
    }

    {
        gamenet::net::EventLoop loop;
        gamenet::game_session::SessionManager sessions(&loop);
        auto firstEndpoint = std::make_shared<BindingEndpoint>(10, &loop);
        auto replacementEndpoint = std::make_shared<BindingEndpoint>(11, &loop);
        const auto first = sessions.authenticate("reentrant", firstEndpoint);
        const auto binding = first.session->binding();

        int handlerCalls = 0;
        int outputCalls = 0;
        gamenet::game_logic::LogicLoop logic(
            &loop,
            {.tickInterval = 1ms, .maxCommandsPerTick = 8});
        logic.setHandler([&](gamenet::game_logic::GameCommand command) {
            ++handlerCalls;
            const auto rebound = sessions.authenticate("reentrant", replacementEndpoint);
            GAMENET_TEST_ASSERT(
                rebound.status == gamenet::game_session::AuthenticateStatus::Rebound);
            return std::optional{std::move(command)};
        });
        logic.setOutputCallback(
            [&](gamenet::game_logic::GameCommand) { ++outputCalls; });
        logic.start();
        GAMENET_TEST_ASSERT(
            logic.submit(commandFor(binding, "rebind-inside-handler")) ==
            gamenet::game_logic::SubmitResult::Accepted);

        loop.runAfter(5ms, [&] {
            GAMENET_TEST_ASSERT(handlerCalls == 1);
            GAMENET_TEST_ASSERT(outputCalls == 0);
            GAMENET_TEST_ASSERT(logic.queueSnapshot().droppedStale == 1);
            (void)logic.stop();
            loop.quit();
        });
        loop.loop();
    }
}
