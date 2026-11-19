#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "agenticpp/llm_client.hpp"

namespace agenticpp {

class FakeLlmClient final : public LlmClient {
public:
    ChatReply reply;
    std::vector<ChatReply> replies;
    std::size_t calls = 0;

    std::vector<std::string> prompts;

    Result<ChatReply> chat(std::span<const Message> history,
                           std::span<const ToolDef>,
                           const ChatOptions&) override {
        const std::size_t index = calls++;
        for (const Message& message : history) {
            if (message.role == "user") {
                prompts.push_back(message.content);
            }
        }
        if (index < replies.size()) {
            return Result<ChatReply>::ok(replies[index]);
        }
        return Result<ChatReply>::ok(reply);
    }

    Result<void> warmup() override { return Result<void>::ok(); }
};

}  // namespace agenticpp
