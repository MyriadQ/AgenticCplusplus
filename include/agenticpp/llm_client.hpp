#pragma once

#include <span>
#include <string>

#include "agenticpp/result.hpp"
#include "agenticpp/types.hpp"

namespace agenticpp {

struct ChatOptions {
    bool thinking = false;
    std::string reasoning_effort;
    int max_tokens = 256;
};

class LlmClient {
public:
    virtual ~LlmClient() = default;

    virtual Result<ChatReply> chat(std::span<const Message> history,
                                   std::span<const ToolDef> tools,
                                   const ChatOptions& opts = {}) = 0;

    virtual Result<void> warmup() = 0;
};

}  // namespace agenticpp
