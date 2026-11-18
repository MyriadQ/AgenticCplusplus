#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agenticpp/deepseek_client.hpp"

namespace {

constexpr std::string_view kGoldenToolCall = R"({
  "choices": [{
    "message": {
      "content": "",
      "tool_calls": [{
        "id": "call_1",
        "type": "function",
        "function": {
          "name": "run_command",
          "arguments": "{\"argv\":[\"scripts/kill-decoy.sh\"],\"purpose\":\"free port 8080\"}"
        }
      }]
    },
    "finish_reason": "tool_calls"
  }],
  "usage": {
    "prompt_tokens": 10,
    "completion_tokens": 5,
    "prompt_cache_hit_tokens": 3
  }
})";

constexpr std::string_view kLengthWithoutToolCall = R"({
  "choices": [{
    "message": {"content": "partial", "tool_calls": []},
    "finish_reason": "length"
  }]
})";

}  // namespace

TEST_CASE("DeepSeek: parses a golden tool call") {
    const auto reply = agenticpp::parse_chat_reply(kGoldenToolCall);

    REQUIRE(reply.has_value());
    REQUIRE(reply.value().finish_reason == "tool_calls");
    REQUIRE(reply.value().tool_calls.size() == 1);
    REQUIRE(reply.value().tool_calls.front().id == "call_1");
    REQUIRE(reply.value().tool_calls.front().name == "run_command");
    REQUIRE(reply.value().tool_calls.front().arguments_json.find("kill-decoy.sh") !=
            std::string::npos);
    REQUIRE(reply.value().usage.prompt_tokens == 10);
    REQUIRE(reply.value().usage.completion_tokens == 5);
    REQUIRE(reply.value().usage.cached_tokens == 3);
}

TEST_CASE("DeepSeek: length without a tool call is rejected") {
    const auto reply = agenticpp::parse_chat_reply(kLengthWithoutToolCall);

    REQUIRE_FALSE(reply.has_value());
    REQUIRE(reply.error().code == 422);
}

TEST_CASE("DeepSeek: missing key returns a typed error") {
    ::unsetenv("AGENTICPP_MISSING_KEY");

    agenticpp::LlmConfig config;
    config.api_key_env = "AGENTICPP_MISSING_KEY";
    agenticpp::DeepSeekClient client(config);

    const std::span<const agenticpp::Message> history;
    const std::span<const agenticpp::ToolDef> tools;
    const auto reply = client.chat(history, tools, {});

    REQUIRE_FALSE(reply.has_value());
    REQUIRE(reply.error().code == 401);
}

TEST_CASE("DeepSeek: malformed JSON is a typed error") {
    const auto reply = agenticpp::parse_chat_reply("not json");

    REQUIRE_FALSE(reply.has_value());
    REQUIRE(reply.error().code == 2);
}

TEST_CASE("DeepSeek: retries a 429 and then succeeds") {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    int reuse = 1;
    (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listen_fd, 2) == 0);

    socklen_t address_size = sizeof(address);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
    const int port = static_cast<int>(ntohs(address.sin_port));

    std::atomic<int> requests{0};
    std::jthread server([listen_fd, &requests](std::stop_token) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            pollfd ready{listen_fd, POLLIN, 0};
            if (::poll(&ready, 1, 3000) <= 0) {
                break;
            }
            const int client = ::accept(listen_fd, nullptr, nullptr);
            if (client < 0) {
                break;
            }

            char request[4096];
            (void)::recv(client, request, sizeof(request) - 1, 0);
            ++requests;

            const std::string body = attempt == 0 ? "{}" : std::string{kGoldenToolCall};
            const std::string status = attempt == 0 ? "429 Too Many Requests" : "200 OK";
            const std::string response = "HTTP/1.1 " + status +
                                         "\r\nContent-Type: application/json\r\nContent-Length: " +
                                         std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
            ::close(client);
        }
        ::close(listen_fd);
    });

    ::setenv("AGENTICPP_RETRY_KEY", "test", 1);

    agenticpp::LlmConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port) + "/chat";
    config.api_key_env = "AGENTICPP_RETRY_KEY";
    config.max_retries = 1;
    config.timeout_ms = 2000;
    config.connect_timeout_ms = 1000;
    config.audit_path = "";
    config.warmup = false;

    agenticpp::DeepSeekClient client(config);
    const std::span<const agenticpp::Message> history;
    const std::span<const agenticpp::ToolDef> tools;
    const auto reply = client.chat(history, tools, {});

    server.join();
    ::unsetenv("AGENTICPP_RETRY_KEY");

    REQUIRE(reply.has_value());
    REQUIRE(requests.load() == 2);
    REQUIRE(reply.value().tool_calls.size() == 1);
}
