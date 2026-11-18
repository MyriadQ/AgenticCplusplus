#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "agenticpp/result.hpp"
#include "agenticpp/types.hpp"

namespace agenticpp {

struct LlmConfig {
    std::string base_url = "https://api.deepseek.com/chat/completions";
    std::string model = "deepseek-flash";
    std::string api_key_env = "DEEPSEEK_API_KEY";
    std::string system_prompt;
    double temperature = 0.0;
    bool thinking = false;
    std::string reasoning_effort = "low";
    int max_tokens = 256;
    int max_tokens_thinking = 2048;
    int timeout_ms = 15000;
    int connect_timeout_ms = 3000;
    int max_retries = 3;
    bool warmup = true;
    int warmup_timeout_ms = 30000;
    std::filesystem::path audit_path = "data/audit.log";
};

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
