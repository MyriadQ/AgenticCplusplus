#pragma once

#include <cstddef>
#include <span>

#include "agenticpp/llm_client.hpp"

namespace agenticpp {

class FakeLlmClient final : public LlmClient {
public:
    ChatReply reply;
    std::size_t calls = 0;

    Result<ChatReply> chat(std::span<const Message>, std::span<const ToolDef>,
                           const ChatOptions&) override {
        ++calls;
        return Result<ChatReply>::ok(reply);
    }

    Result<void> warmup() override { return Result<void>::ok(); }
};

}  // namespace agenticpp
