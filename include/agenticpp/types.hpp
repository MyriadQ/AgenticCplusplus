#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agenticpp {

using Timestamp = std::chrono::system_clock::time_point;

struct Error {
    int code = 0;
    std::string message;
};

enum class Phase {
    Idle,
    Detect,
    Recall,
    Diagnose,
    Propose,
    Authorize,
    Act,
    Verify,
    Learn,
    Cooldown,
    Done,
};

constexpr std::string_view phase_name(Phase phase) noexcept {
    switch (phase) {
        case Phase::Idle:
            return "Idle";
        case Phase::Detect:
            return "Detect";
        case Phase::Recall:
            return "Recall";
        case Phase::Diagnose:
            return "Diagnose";
        case Phase::Propose:
            return "Propose";
        case Phase::Authorize:
            return "Authorize";
        case Phase::Act:
            return "Act";
        case Phase::Verify:
            return "Verify";
        case Phase::Learn:
            return "Learn";
        case Phase::Cooldown:
            return "Cooldown";
        case Phase::Done:
            return "Done";
    }
    return "Unknown";
}

struct Transition {
    Phase from = Phase::Idle;
    Phase to = Phase::Idle;
    std::string reason;
    Timestamp at{};
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

struct Message {
    std::string role;  // "system" | "user" | "assistant" | "tool"
    std::string content;
    std::string tool_call_id;         // role == "tool"
    std::vector<ToolCall> tool_calls; // role == "assistant"
    std::string reasoning_content;    // thinking mode only
};

struct ToolDef {
    std::string name;
    std::string description;
    std::string parameters_json;
};

struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int reasoning_tokens = 0;
    int cached_tokens = 0;
};

struct ChatReply {
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason; // "stop" | "tool_calls" | "length" | ...
    Usage usage;
};

struct Rule {
    std::string id;
    std::string program;
    std::string doc;
    std::size_t max_args = 0;
};

struct ExecLimits {
    std::size_t output_cap = 64 * 1024;
    int cpu_seconds = 10;
    std::uint64_t address_space = 512ull << 20;
    int nproc = 64;
    std::uint64_t fsize = 1ull << 20;
};

struct ExecResult {
    int exit_code = -1; // -1 if signalled
    int term_signal = 0;
    bool timed_out = false;
    bool truncated = false;
    std::string out;
    std::string err;
    std::chrono::microseconds elapsed{};
};

struct Check {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct Verdict {
    bool ok = false;
    std::vector<Check> checks;
};

struct Event {
    Timestamp at{};
    std::string phase;
    std::string detail;
};

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(const Event& event) = 0;
};

struct AgentConfig {
    std::size_t max_tool_calls = 4;
    std::size_t max_llm_round_trips = 3;
    std::size_t max_concurrent_incidents = 2;
    std::chrono::seconds dedup_window{60};
    std::chrono::seconds cooldown_initial{60};
    std::chrono::seconds cooldown_max{1800};
    std::chrono::milliseconds llm_budget{10000};
    std::chrono::milliseconds llm_budget_escalated{25000};
};

}  // namespace agenticpp
