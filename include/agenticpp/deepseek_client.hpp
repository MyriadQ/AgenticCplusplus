#pragma once

#include <memory>
#include <string_view>

#include "agenticpp/llm_client.hpp"

namespace agenticpp {

// Parses an OpenAI-compatible response body. Exposed for golden-file tests.
Result<ChatReply> parse_chat_reply(std::string_view body);

class DeepSeekClient final : public LlmClient {
public:
    using Config = LlmConfig;

    explicit DeepSeekClient(Config config);
    ~DeepSeekClient() override;

    DeepSeekClient(const DeepSeekClient&) = delete;
    DeepSeekClient& operator=(const DeepSeekClient&) = delete;

    Result<ChatReply> chat(std::span<const Message> history, std::span<const ToolDef> tools,
                           const ChatOptions& opts = {}) override;

    Result<void> warmup() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace agenticpp
