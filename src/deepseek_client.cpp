#if AGENTICPP_ENABLE_LLM

#include "agenticpp/deepseek_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <simdjson.h>

#include "agenticpp/template_parser.hpp"

namespace agenticpp {
namespace {

constexpr std::size_t kMaxErrorBody = 512;

std::size_t write_callback(char* data, std::size_t size, std::size_t count, void* user) {
    auto* output = static_cast<std::string*>(user);
    output->append(data, size * count);
    return size * count;
}

bool retryable_http(long status) {
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 ||
           status == 504;
}

bool retryable_curl(CURLcode code) {
    return code != CURLE_URL_MALFORMAT && code != CURLE_UNSUPPORTED_PROTOCOL;
}

void append_audit(const LlmConfig& config, const std::string& request_body, bool thinking,
                  const ChatReply& reply, std::chrono::milliseconds total) {
    if (config.audit_path.empty()) {
        return;
    }

    nlohmann::json line;
    line["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    line["model"] = config.model;
    line["thinking"] = thinking;
    line["prompt_hash"] = std::to_string(fnv1a64(request_body));
    line["ttft_ms"] = 0; // libcurl easy handle does not expose first-byte timing here
    line["total_ms"] = total.count();
    line["prompt_tokens"] = reply.usage.prompt_tokens;
    line["completion_tokens"] = reply.usage.completion_tokens;
    line["reasoning_tokens"] = reply.usage.reasoning_tokens;
    line["cached_tokens"] = reply.usage.cached_tokens;
    line["finish_reason"] = reply.finish_reason;
    line["tool"] = reply.tool_calls.empty() ? "" : reply.tool_calls.front().name;
    line["decision"] = reply.tool_calls.empty() ? "no_tool_call" : "tool_call";

    std::error_code error;
    if (!config.audit_path.parent_path().empty()) {
        std::filesystem::create_directories(config.audit_path.parent_path(), error);
    }
    std::ofstream output(config.audit_path, std::ios::app);
    if (output) {
        output << line.dump() << '\n';
    }
}

}  // namespace

Result<ChatReply> parse_chat_reply(std::string_view body) {
    simdjson::dom::parser parser;
    simdjson::padded_string padded{body};
    simdjson::dom::element document;
    const simdjson::error_code parse_error = parser.parse(padded).get(document);
    if (parse_error != simdjson::SUCCESS) {
        return Result<ChatReply>::err(2, std::string{"invalid LLM JSON: "} +
                                             simdjson::error_message(parse_error));
    }

    simdjson::dom::array choices;
    const simdjson::error_code choices_error = document["choices"].get_array().get(choices);
    if (choices_error != simdjson::SUCCESS || choices.size() == 0) {
        return Result<ChatReply>::err(3, "LLM response has no choices");
    }

    ChatReply reply;
    simdjson::dom::element choice = *choices.begin();
    simdjson::dom::element message = choice["message"];

    std::string_view text;
    if (message["content"].get_string().get(text) == simdjson::SUCCESS) {
        reply.content = std::string{text};
    }

    if (choice["finish_reason"].get_string().get(text) == simdjson::SUCCESS) {
        reply.finish_reason = std::string{text};
    }

    simdjson::dom::array tool_calls;
    if (message["tool_calls"].get_array().get(tool_calls) == simdjson::SUCCESS) {
        for (auto item : tool_calls) {
            ToolCall call;
            if (item["id"].get_string().get(text) == simdjson::SUCCESS) {
                call.id = std::string{text};
            }
            simdjson::dom::element function = item["function"];
            if (function["name"].get_string().get(text) == simdjson::SUCCESS) {
                call.name = std::string{text};
            }
            if (function["arguments"].get_string().get(text) == simdjson::SUCCESS) {
                call.arguments_json = std::string{text};
            }
            reply.tool_calls.push_back(std::move(call));
        }
    }

    simdjson::dom::element usage;
    if (document["usage"].get(usage) == simdjson::SUCCESS) {
        std::int64_t number = 0;
        if (usage["prompt_tokens"].get_int64().get(number) == simdjson::SUCCESS) {
            reply.usage.prompt_tokens = static_cast<int>(number);
        }
        if (usage["completion_tokens"].get_int64().get(number) == simdjson::SUCCESS) {
            reply.usage.completion_tokens = static_cast<int>(number);
        }
        if (usage["prompt_cache_hit_tokens"].get_int64().get(number) == simdjson::SUCCESS) {
            reply.usage.cached_tokens = static_cast<int>(number);
        }
        simdjson::dom::element details;
        if (usage["completion_tokens_details"].get(details) == simdjson::SUCCESS &&
            details["reasoning_tokens"].get_int64().get(number) == simdjson::SUCCESS) {
            reply.usage.reasoning_tokens = static_cast<int>(number);
        }
    }

    if (reply.finish_reason == "length" && reply.tool_calls.empty()) {
        return Result<ChatReply>::err(422, "LLM output was truncated before a tool call");
    }
    return Result<ChatReply>::ok(std::move(reply));
}

struct DeepSeekClient::Impl {
    LlmConfig config;
    std::string api_key;
    CURL* curl = nullptr;

    explicit Impl(LlmConfig settings) : config(std::move(settings)) {
        if (const char* key = std::getenv(config.api_key_env.c_str()); key != nullptr) {
            api_key = key;
        }
        (void)::curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = ::curl_easy_init();
    }

    ~Impl() {
        if (curl != nullptr) {
            ::curl_easy_cleanup(curl);
        }
        ::curl_global_cleanup();
    }

    Result<ChatReply> perform(const std::string& body, bool thinking, bool audit, int timeout_ms) {
        if (api_key.empty()) {
            return Result<ChatReply>::err(401, "missing API key in " + config.api_key_env);
        }
        if (curl == nullptr) {
            return Result<ChatReply>::err(500, "libcurl initialization failed");
        }

        const auto started = std::chrono::steady_clock::now();
        CURLcode code = CURLE_OK;
        long status = 0;
        std::string response;

        const int attempts = std::max(1, config.max_retries + 1);
        for (int attempt = 0; attempt < attempts; ++attempt) {
            response.clear();
            status = 0;

            ::curl_easy_reset(curl);
            curl_slist* headers = nullptr;
            headers = ::curl_slist_append(headers, "Content-Type: application/json");
            const std::string authorization = "Authorization: Bearer " + api_key;
            headers = ::curl_slist_append(headers, authorization.c_str());

            ::curl_easy_setopt(curl, CURLOPT_URL, config.base_url.c_str());
            ::curl_easy_setopt(curl, CURLOPT_POST, 1L);
            ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
            ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            ::curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
            ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                               static_cast<long>(config.connect_timeout_ms));

            code = ::curl_easy_perform(curl);
            ::curl_slist_free_all(headers);
            if (code == CURLE_OK) {
                ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
                if (status == 200) {
                    break;
                }
                if (!retryable_http(status)) {
                    break;
                }
            } else if (!retryable_curl(code)) {
                break;
            }

            if (attempt + 1 < attempts) {
                const int base = 500 * (1 << attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds{std::min(base, 2000)});
            }
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        if (code != CURLE_OK) {
            return Result<ChatReply>::err(502, std::string{"curl: "} + ::curl_easy_strerror(code));
        }
        if (status != 200) {
            std::string detail = response.substr(0, std::min(response.size(), kMaxErrorBody));
            return Result<ChatReply>::err(static_cast<int>(status),
                                          "LLM HTTP " + std::to_string(status) + ": " + detail);
        }

        auto parsed = parse_chat_reply(response);
        if (!parsed.has_value()) {
            return parsed;
        }
        if (audit) {
            append_audit(config, body, thinking, parsed.value(), elapsed);
        }
        return parsed;
    }
};

DeepSeekClient::DeepSeekClient(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

DeepSeekClient::~DeepSeekClient() = default;

Result<ChatReply> DeepSeekClient::chat(std::span<const Message> history,
                                       std::span<const ToolDef> tools,
                                       const ChatOptions& opts) {
    nlohmann::json request;
    request["model"] = impl_->config.model;
    request["temperature"] = impl_->config.temperature;
    request["stream"] = false;

    const bool thinking = opts.thinking;
    request["thinking"] = nlohmann::json{{"type", thinking ? "enabled" : "disabled"}};
    if (thinking && !opts.reasoning_effort.empty()) {
        request["reasoning_effort"] = opts.reasoning_effort;
    }
    request["max_tokens"] = thinking ? impl_->config.max_tokens_thinking
                                     : (opts.max_tokens > 0 ? opts.max_tokens
                                                            : impl_->config.max_tokens);

    nlohmann::json messages = nlohmann::json::array();
    for (const Message& message : history) {
        nlohmann::json item;
        item["role"] = message.role;
        item["content"] = message.content;
        if (!message.tool_call_id.empty()) {
            item["tool_call_id"] = message.tool_call_id;
        }
        if (!message.reasoning_content.empty()) {
            item["reasoning_content"] = message.reasoning_content;
        }
        if (!message.tool_calls.empty()) {
            item["tool_calls"] = nlohmann::json::array();
            for (const ToolCall& call : message.tool_calls) {
                item["tool_calls"].push_back(
                    {{"id", call.id},
                     {"type", "function"},
                     {"function", {{"name", call.name}, {"arguments", call.arguments_json}}}});
            }
        }
        messages.push_back(std::move(item));
    }
    request["messages"] = std::move(messages);

    if (!tools.empty()) {
        nlohmann::json definitions = nlohmann::json::array();
        for (const ToolDef& tool : tools) {
            nlohmann::json parameters = nlohmann::json::object();
            try {
                parameters = nlohmann::json::parse(tool.parameters_json);
            } catch (const std::exception&) {
                parameters = nlohmann::json::object();
            }
            definitions.push_back(
                {{"type", "function"},
                 {"function",
                  {{"name", tool.name},
                   {"description", tool.description},
                   {"parameters", std::move(parameters)}}}});
        }
        request["tools"] = std::move(definitions);
        request["tool_choice"] = "required";
    }

    return impl_->perform(request.dump(), thinking, true, impl_->config.timeout_ms);
}

Result<void> DeepSeekClient::warmup() {
    if (!impl_->config.warmup) {
        return Result<void>::ok();
    }

    nlohmann::json request;
    request["model"] = impl_->config.model;
    request["temperature"] = 0.0;
    request["stream"] = false;
    request["max_tokens"] = 64;
    request["thinking"] = nlohmann::json{{"type", "disabled"}};
    request["messages"] = nlohmann::json::array(
        {{{"role", "system"}, {"content", impl_->config.system_prompt}},
         {{"role", "user"}, {"content", "Reply with the single word READY."}}});

    auto result = impl_->perform(request.dump(), false, false, impl_->config.warmup_timeout_ms);
    if (!result.has_value()) {
        return Result<void>::err(result.error().code, result.error().message);
    }
    return Result<void>::ok();
}

}  // namespace agenticpp

#endif  // AGENTICPP_ENABLE_LLM
