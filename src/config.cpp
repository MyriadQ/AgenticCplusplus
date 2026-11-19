#include "agenticpp/config.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "agenticpp/vectorizer.hpp"

namespace agenticpp {
namespace {

int positive(int value, int fallback) {
    return value > 0 ? value : fallback;
}

}  // namespace

Result<AppConfig> load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return Result<AppConfig>::err(1, "cannot open config: " + path.string());
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const std::exception& error) {
        return Result<AppConfig>::err(2, std::string{"invalid config JSON: "} + error.what());
    }
    if (!json.is_object()) {
        return Result<AppConfig>::err(3, "config root must be an object");
    }

    AppConfig config;
    config.mode = json.value("mode", config.mode);
    config.require_confirmation = json.value("require_confirmation", config.require_confirmation);

    if (json.contains("log_source") && json["log_source"].is_object()) {
        const auto& source = json["log_source"];
        config.log_source.path = source.value("path", config.log_source.path);
        config.log_source.poll_interval_ms =
            positive(source.value("poll_interval_ms", config.log_source.poll_interval_ms),
                     config.log_source.poll_interval_ms);
        config.log_source.from_start = source.value("from_start", config.log_source.from_start);
        const int max_line = source.value("max_line_bytes", 65536);
        if (max_line > 0) {
            config.log_source.max_line_bytes = static_cast<std::size_t>(max_line);
        }
    }

    if (json.contains("memory") && json["memory"].is_object()) {
        const auto& memory = json["memory"];
        config.memory.path = memory.value("path", config.memory.path);
        config.memory.journal_path = memory.value("journal_path", config.memory.journal_path);
        config.memory.dim = memory.value("dim", config.memory.dim);
        const int compact_after =
            memory.value("compact_after_events", static_cast<int>(config.memory.compact_after_events));
        if (compact_after >= 0) {
            config.memory.compact_after_events = static_cast<std::size_t>(compact_after);
        }
        const int top_k = memory.value("top_k", static_cast<int>(config.memory.top_k));
        if (top_k > 0) {
            config.memory.top_k = static_cast<std::size_t>(top_k);
        }
        config.memory.recall_threshold = static_cast<float>(
            memory.value("recall_threshold", static_cast<double>(config.memory.recall_threshold)));
        if (config.memory.dim != static_cast<int>(Vectorizer::kDim)) {
            return Result<AppConfig>::err(4, "memory.dim must match Vectorizer::kDim");
        }
    }

    if (json.contains("llm") && json["llm"].is_object()) {
        const auto& llm = json["llm"];
        config.llm.base_url = llm.value("base_url", config.llm.base_url);
        config.llm.model = llm.value("model", config.llm.model);
        config.llm.api_key_env = llm.value("api_key_env", config.llm.api_key_env);
        config.llm.temperature = llm.value("temperature", config.llm.temperature);
        config.llm.thinking = llm.value("thinking", config.llm.thinking);
        config.llm.reasoning_effort = llm.value("reasoning_effort", config.llm.reasoning_effort);
        config.llm.max_tokens = positive(llm.value("max_tokens", config.llm.max_tokens),
                                         config.llm.max_tokens);
        config.llm.max_tokens_thinking =
            positive(llm.value("max_tokens_thinking", config.llm.max_tokens_thinking),
                     config.llm.max_tokens_thinking);
        config.llm.timeout_ms =
            positive(llm.value("timeout_ms", config.llm.timeout_ms), config.llm.timeout_ms);
        config.llm.connect_timeout_ms = positive(
            llm.value("connect_timeout_ms", config.llm.connect_timeout_ms),
            config.llm.connect_timeout_ms);
        config.llm.max_retries = llm.value("max_retries", config.llm.max_retries);
        config.llm.warmup = llm.value("warmup", config.llm.warmup);
        config.llm.warmup_timeout_ms =
            positive(llm.value("warmup_timeout_ms", config.llm.warmup_timeout_ms),
                     config.llm.warmup_timeout_ms);
    }

    if (json.contains("guards") && json["guards"].is_object()) {
        const auto& guards = json["guards"];
        config.agent.max_tool_calls =
            static_cast<std::size_t>(positive(static_cast<int>(config.agent.max_tool_calls),
                                              4));
        config.agent.max_tool_calls = static_cast<std::size_t>(guards.value(
            "max_tool_calls_per_incident", static_cast<int>(config.agent.max_tool_calls)));
        config.agent.max_llm_round_trips = static_cast<std::size_t>(guards.value(
            "max_llm_round_trips", static_cast<int>(config.agent.max_llm_round_trips)));
        const int max_attempts = guards.value("max_repair_attempts",
                                              static_cast<int>(config.agent.max_repair_attempts));
        if (max_attempts > 0) {
            config.agent.max_repair_attempts = static_cast<std::size_t>(max_attempts);
        }
        config.agent.max_concurrent_incidents = static_cast<std::size_t>(guards.value(
            "max_concurrent_incidents", static_cast<int>(config.agent.max_concurrent_incidents)));
        config.agent.dedup_window = std::chrono::seconds{
            guards.value("incident_dedup_window_s",
                         static_cast<int>(config.agent.dedup_window.count()))};
        config.agent.cooldown_initial = std::chrono::seconds{
            guards.value("cooldown_initial_s",
                         static_cast<int>(config.agent.cooldown_initial.count()))};
        config.agent.cooldown_max = std::chrono::seconds{
            guards.value("cooldown_max_s", static_cast<int>(config.agent.cooldown_max.count()))};
        config.agent.llm_budget = std::chrono::milliseconds{
            guards.value("max_llm_wall_ms", static_cast<int>(config.agent.llm_budget.count()))};
        config.agent.llm_budget_escalated = std::chrono::milliseconds{
            guards.value("max_llm_wall_ms_escalated",
                         static_cast<int>(config.agent.llm_budget_escalated.count()))};
        config.agent.recall_threshold = config.memory.recall_threshold;
    }

    if (json.contains("executor") && json["executor"].is_object()) {
        const auto& executor = json["executor"];
        config.executor.timeout = std::chrono::milliseconds{
            positive(executor.value("timeout_ms", static_cast<int>(config.executor.timeout.count())),
                     static_cast<int>(config.executor.timeout.count()))};
        config.executor.grace = std::chrono::milliseconds{
            positive(executor.value("grace_ms", static_cast<int>(config.executor.grace.count())),
                     static_cast<int>(config.executor.grace.count()))};
        const int output_cap = executor.value("output_cap_bytes", 65536);
        if (output_cap > 0) {
            config.executor.output_cap = static_cast<std::size_t>(output_cap);
        }
        if (executor.contains("env_allowlist") && executor["env_allowlist"].is_array()) {
            config.executor.env_allowlist.clear();
            for (const auto& item : executor["env_allowlist"]) {
                if (item.is_string()) {
                    config.executor.env_allowlist.push_back(item.get<std::string>());
                }
            }
        }
        if (executor.contains("sandbox") && executor["sandbox"].is_object()) {
            const auto& sandbox = executor["sandbox"];
            const int tier = sandbox.value("tier", config.executor.sandbox_tier);
            if (tier < 0) {
                return Result<AppConfig>::err(5, "executor.sandbox.tier must be >= 0");
            }
            config.executor.sandbox_tier = tier;
            config.executor.sandbox_network =
                sandbox.value("network", config.executor.sandbox_network);
            if (sandbox.contains("writable_paths") && sandbox["writable_paths"].is_array()) {
                config.executor.sandbox_writable_paths.clear();
                for (const auto& item : sandbox["writable_paths"]) {
                    if (item.is_string()) {
                        config.executor.sandbox_writable_paths.emplace_back(
                            item.get<std::string>());
                    }
                }
            }
        }
        if (executor.contains("rlimits") && executor["rlimits"].is_object()) {
            const auto& limits = executor["rlimits"];
            config.executor.limits.cpu_seconds =
                positive(limits.value("cpu_seconds", config.executor.limits.cpu_seconds),
                         config.executor.limits.cpu_seconds);
            config.executor.limits.nproc =
                positive(limits.value("nproc", config.executor.limits.nproc),
                         config.executor.limits.nproc);
            const auto address_space =
                limits.value("address_space_bytes",
                             static_cast<std::uint64_t>(config.executor.limits.address_space));
            if (address_space > 0) {
                config.executor.limits.address_space = address_space;
            }
            const auto fsize = limits.value(
                "fsize_bytes", static_cast<std::uint64_t>(config.executor.limits.fsize));
            if (fsize > 0) {
                config.executor.limits.fsize = fsize;
            }
        }
    }

    if (json.contains("verifier") && json["verifier"].is_object()) {
        const auto& verifier = json["verifier"];
        config.verifier.health_url = verifier.value("health_url", config.verifier.health_url);
        config.verifier.health_timeout = std::chrono::milliseconds{positive(
            verifier.value("health_timeout_ms",
                           static_cast<int>(config.verifier.health_timeout.count())),
            static_cast<int>(config.verifier.health_timeout.count()))};
        config.verifier.log_quiet_window = std::chrono::milliseconds{positive(
            verifier.value("log_quiet_window_ms",
                           static_cast<int>(config.verifier.log_quiet_window.count())),
            static_cast<int>(config.verifier.log_quiet_window.count()))};
    }

    return Result<AppConfig>::ok(std::move(config));
}

}  // namespace agenticpp
