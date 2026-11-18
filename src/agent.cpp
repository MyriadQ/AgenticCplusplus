#include "agenticpp/agent.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agenticpp/template_parser.hpp"

namespace agenticpp {
namespace {

struct Proposal {
    std::vector<std::string> argv;
    std::string purpose;
};

bool is_actionable(std::string_view line) {
    return line.find("error") != std::string_view::npos ||
           line.find("Error") != std::string_view::npos ||
           line.find("ERROR") != std::string_view::npos ||
           line.find("fatal") != std::string_view::npos ||
           line.find("FATAL") != std::string_view::npos ||
           line.find("bind:") != std::string_view::npos;
}

std::string hex_id(std::uint64_t id) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(id));
    return buffer;
}

std::string display_template(std::string_view text) {
    return std::string{text.substr(0, 200)};
}

std::string build_system_prompt(const Policy& policy) {
    std::string prompt =
        "You are a bounded log-repair agent. Choose exactly one allowlisted program via the "
        "run_command tool. Never use a shell, diagnostics, or any program not listed.\n"
        "Allowed programs:\n";
    for (const Rule& rule : policy.rules()) {
        prompt += "- " + rule.program + " (id=" + rule.id +
                  ", max_args=" + std::to_string(rule.max_args) + "): " + rule.doc + "\n";
    }
    prompt +=
        "Pick the program whose description matches the incident. argv[0] must be the listed "
        "program path; add at most max_args arguments.";
    return prompt;
}

std::string join_argv(const std::vector<std::string>& argv) {
    std::string text = "[";
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += argv[i];
    }
    text += "]";
    return text;
}

std::optional<Proposal> proposal_from_reply(const ChatReply& reply) {
    for (const ToolCall& call : reply.tool_calls) {
        if (call.name != "run_command") {
            continue;
        }

        try {
            const nlohmann::json arguments = nlohmann::json::parse(call.arguments_json);
            if (!arguments.is_object() || !arguments.contains("argv") ||
                !arguments["argv"].is_array()) {
                return std::nullopt;
            }

            Proposal proposal;
            for (const auto& item : arguments["argv"]) {
                if (item.is_string()) {
                    proposal.argv.push_back(item.get<std::string>());
                }
            }
            if (proposal.argv.empty()) {
                return std::nullopt;
            }
            proposal.purpose = arguments.value("purpose", std::string{});
            return proposal;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

Agent::Agent(AgentConfig config, MemoryStore& memory, LlmClient& llm, Policy& policy,
             CommandRunner& runner, Verifier& verifier, EventSink& sink)
    : config_(config),
      memory_(memory),
      llm_(llm),
      policy_(policy),
      runner_(runner),
      verifier_(verifier),
      sink_(sink) {}

void Agent::on_line(std::string_view line) {
    if (!is_actionable(line)) {
        pending_line_.reset();
        return;
    }
    pending_line_ = std::string(line);
}

void Agent::run_once() {
    if (!pending_line_.has_value()) {
        return;
    }

    const std::string line = std::move(*pending_line_);
    pending_line_.reset();
    run_incident(line);
}

const std::vector<Transition>& Agent::trail() const noexcept {
    return trail_;
}

void Agent::transition(Phase from, Phase to, std::string reason) {
    const Timestamp at = std::chrono::system_clock::now();
    trail_.push_back(Transition{from, to, reason, at});
    sink_.emit(Event{at, std::string(phase_name(to)), reason});
}

void Agent::run_incident(std::string_view line) {
    trail_.clear();

    const ParsedLine parsed = parse_template(line);
    transition(Phase::Idle, Phase::Detect,
               "template=" + hex_id(parsed.template_id) + " \"" +
                   display_template(parsed.template_text) + "\"");

    transition(Phase::Detect, Phase::Recall, "querying memory");
    const std::vector<Hit> hits = memory_.search(parsed.template_text, 1);

    std::vector<std::string> argv;
    std::string purpose;

    const bool recall_hit = !hits.empty() && hits.front().record != nullptr &&
                            hits.front().score >= config_.recall_threshold;

    if (recall_hit) {
        argv = hits.front().record->argv;
        purpose = hits.front().record->purpose;
        transition(Phase::Recall, Phase::Propose,
                   "recall hit score=" + std::to_string(hits.front().score));
    } else {
        std::string reason = "recall miss";
        if (!hits.empty()) {
            reason += " best=" + std::to_string(hits.front().score) + " threshold=" +
                      std::to_string(config_.recall_threshold);
        }
        transition(Phase::Recall, Phase::Diagnose, reason);

        std::vector<Message> history;
        Message system_message;
        system_message.role = "system";
        system_message.content = build_system_prompt(policy_);
        history.push_back(std::move(system_message));

        Message user_message;
        user_message.role = "user";
        user_message.content = std::string(line);
        history.push_back(std::move(user_message));

        std::vector<ToolDef> tools;
        ToolDef tool;
        tool.name = "run_command";
        tool.description = "Execute one allowlisted remediation command.";
        tool.parameters_json =
            R"({"type":"object","properties":{"argv":{"type":"array","items":{"type":"string"}},"purpose":{"type":"string"}},"required":["argv","purpose"]})";
        tools.push_back(std::move(tool));

        const auto reply = llm_.chat(std::span<const Message>{history},
                                    std::span<const ToolDef>{tools}, ChatOptions{});
        if (!reply.has_value()) {
            transition(Phase::Diagnose, Phase::Cooldown,
                       "llm error: " + reply.error().message);
            return;
        }

        const std::optional<Proposal> proposal = proposal_from_reply(reply.value());
        if (!proposal.has_value()) {
            transition(Phase::Diagnose, Phase::Cooldown,
                       "llm returned no usable run_command");
            return;
        }

        argv = proposal->argv;
        purpose = proposal->purpose;
        transition(Phase::Diagnose, Phase::Propose, "proposal " + join_argv(argv));
    }

    transition(Phase::Propose, Phase::Authorize, "validate proposal");
    const PolicyVerdict verdict = policy_.check(argv);
    if (verdict.decision != Decision::Allow) {
        transition(Phase::Authorize, Phase::Cooldown, "denied: " + verdict.reason);
        return;
    }
    transition(Phase::Authorize, Phase::Act, "allow rule=" + verdict.rule_id);

    if (!verdict.resolved_program.empty()) {
        argv[0] = verdict.resolved_program.string();
    }

    const ExecResult result = runner_.run(argv);
    const std::string command_detail = "exit=" + std::to_string(result.exit_code) +
                                       (result.timed_out ? " timed_out" : "") +
                                       (result.err.empty() ? "" : " err=" + result.err.substr(0, 200));
    if (result.exit_code != 0 || result.timed_out) {
        transition(Phase::Act, Phase::Cooldown, "command failed " + command_detail);
        return;
    }
    transition(Phase::Act, Phase::Verify, "command " + command_detail);

    const Verdict verification = verifier_.verify(parsed.template_id);
    if (!verification.ok) {
        transition(Phase::Verify, Phase::Cooldown, "verification failed");
        return;
    }
    transition(Phase::Verify, Phase::Learn, "verification passed");

    FixRecord record;
    record.argv = argv;
    record.purpose = purpose;
    record.successes = 1;
    record.last_seen = std::chrono::system_clock::now();
    memory_.upsert(parsed.template_id, parsed.template_text, std::move(record));

    transition(Phase::Learn, Phase::Done, "stored fix for " + hex_id(parsed.template_id));
}

}  // namespace agenticpp
