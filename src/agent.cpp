#include "agenticpp/agent.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

struct AttemptFailure {
    std::string kind;
    std::string detail;
    std::vector<std::string> argv;
    std::vector<Check> checks;
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

Timestamp now() {
    return std::chrono::system_clock::now();
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
        "program path; add at most max_args arguments. If a previous attempt is shown, do not "
        "repeat its argv; choose a different allowlisted repair.";
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

std::string format_checks(std::span<const Check> checks) {
    std::string text;
    for (const Check& check : checks) {
        text += "\n  - " + check.name + "=" + (check.ok ? "ok" : "failed");
        if (!check.detail.empty()) {
            text += ": " + check.detail;
        }
    }
    return text;
}

std::string build_user_message(std::string_view line,
                               const std::optional<AttemptFailure>& previous_failure,
                               const SkillResult& skill) {
    std::string text = "Incident log line:\n";
    text.append(line);
    if (previous_failure.has_value()) {
        text += "\n\nPrevious repair attempt failed:";
        text += "\nkind: " + previous_failure->kind;
        text += "\nargv: " + join_argv(previous_failure->argv);
        text += "\ndetail: " + previous_failure->detail;
        if (!previous_failure->checks.empty()) {
            text += "\nchecks:" + format_checks(previous_failure->checks);
        }
        text += "\nChoose a different allowlisted repair for the original incident.";
    }
    if (!skill.evidence.empty()) {
        text += "\n\nLocal diagnostic evidence:";
        for (const Evidence& evidence : skill.evidence) {
            text += "\n- " + evidence.source + ": " + evidence.detail;
        }
    }
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

// Releases the per-template claim on every early return. A verified repair
// marks success; every other path records a failed attempt and starts a
// cooldown.
class CooldownGuard {
public:
    CooldownGuard(CooldownTable* table, std::uint64_t template_id)
        : table_(table), template_id_(template_id) {}

    ~CooldownGuard() {
        if (table_ == nullptr) {
            return;
        }
        if (success_) {
            table_->finish_success(template_id_, now());
        } else {
            table_->finish_failure(template_id_, now());
        }
    }

    CooldownGuard(const CooldownGuard&) = delete;
    CooldownGuard& operator=(const CooldownGuard&) = delete;

    void mark_success() { success_ = true; }

private:
    CooldownTable* table_;
    std::uint64_t template_id_;
    bool success_ = false;
};

// Emits one Timing event for every incident, including duplicated/cooldown
// paths. The shell demo also times the whole scenario; this event isolates the
// Agent's decision and repair latency.
class IncidentTiming {
public:
    IncidentTiming(EventSink& sink, std::uint64_t template_id)
        : sink_(sink), template_id_(template_id), started_(Clock::now()) {}

    ~IncidentTiming() {
        const auto now = std::chrono::system_clock::now();
        const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started_);
        std::string detail = "template=" + hex_id(template_id_) +
                             " total_ms=" + std::to_string(total.count());
        if (first_proposal_.has_value()) {
            const auto decision = std::chrono::duration_cast<std::chrono::milliseconds>(
                *first_proposal_ - started_);
            detail += " first_proposal_ms=" + std::to_string(decision.count());
        }
        sink_.emit(Event{now, "Timing", std::move(detail)});
    }

    IncidentTiming(const IncidentTiming&) = delete;
    IncidentTiming& operator=(const IncidentTiming&) = delete;

    void mark_proposal() {
        if (!first_proposal_.has_value()) {
            first_proposal_ = Clock::now();
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    EventSink& sink_;
    std::uint64_t template_id_;
    Clock::time_point started_;
    std::optional<Clock::time_point> first_proposal_;
};

}  // namespace

Agent::Agent(AgentConfig config, MemoryStore& memory, LlmClient& llm, Policy& policy,
             CommandRunner& runner, Verifier& verifier, EventSink& sink,
             CooldownTable* cooldown, SkillRegistry* skills)
    : config_(config),
      memory_(memory),
      llm_(llm),
      policy_(policy),
      runner_(runner),
      verifier_(verifier),
      sink_(sink),
      cooldown_(cooldown),
      skills_(skills) {}

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
    IncidentTiming timing{sink_, parsed.template_id};
    transition(Phase::Idle, Phase::Detect,
               "template=" + hex_id(parsed.template_id) + " \"" +
                   display_template(parsed.template_text) + "\"");

    if (cooldown_ != nullptr) {
        const CooldownDecision decision = cooldown_->try_begin(parsed.template_id, now());
        if (!decision.allow) {
            transition(Phase::Detect, Phase::Cooldown, decision.reason);
            return;
        }
    }
    CooldownGuard cooldown_guard{cooldown_, parsed.template_id};

    transition(Phase::Detect, Phase::Recall, "querying memory");
    const std::vector<Hit> hits = memory_.search(parsed.template_text, 1);

    const bool recall_hit = !hits.empty() && hits.front().record != nullptr &&
                            hits.front().score >= config_.recall_threshold;

    const std::size_t llm_attempts =
        config_.max_llm_round_trips == 0 ? 1 : config_.max_llm_round_trips;
    const std::size_t max_attempts =
        std::max<std::size_t>(1, std::min(config_.max_repair_attempts, llm_attempts));
    std::vector<std::vector<std::string>> attempted_argv;
    std::optional<AttemptFailure> previous_failure;
    std::size_t attempt = 0;

    const auto fail = [&](Phase from, std::string kind, std::string detail,
                          std::vector<Check> checks,
                          const std::vector<std::string>& failed_argv) -> bool {
        std::string attempt_text = std::to_string(attempt);
        if (detail.size() > 400) {
            detail.resize(400);
        }
        transition(from, Phase::Replan,
                   "attempt " + attempt_text + " failed: " + kind + ": " + detail);
        if (attempt >= max_attempts) {
            transition(Phase::Replan, Phase::Cooldown,
                       "attempts exhausted after " + attempt_text + ": " + kind + ": " + detail);
            return false;
        }
        previous_failure = AttemptFailure{kind, detail, failed_argv, std::move(checks)};
        return true;
    };

    for (attempt = 1; attempt <= max_attempts; ++attempt) {
        std::vector<std::string> argv;
        std::string purpose;

        if (attempt == 1 && recall_hit) {
            argv = hits.front().record->argv;
            purpose = hits.front().record->purpose;
            timing.mark_proposal();
            transition(Phase::Recall, Phase::Propose,
                       "recall hit score=" + std::to_string(hits.front().score));
        } else {
            if (attempt > 1) {
                transition(Phase::Replan, Phase::Probe,
                           "replan attempt " + std::to_string(attempt) + " with failure feedback");
            } else {
                std::string reason = "recall miss";
                if (!hits.empty()) {
                    reason += " best=" + std::to_string(hits.front().score) + " threshold=" +
                              std::to_string(config_.recall_threshold);
                }
                transition(Phase::Recall, Phase::Probe, reason);
            }

            const SkillContext context{line, parsed};
            const SkillResult skill_result =
                skills_ == nullptr ? SkillResult{} : skills_->probe(context);
            const bool skill_duplicate =
                std::find(attempted_argv.begin(), attempted_argv.end(),
                          skill_result.suggested_argv) != attempted_argv.end();
            const bool use_skill = skill_result.conclusive &&
                                   !skill_result.suggested_argv.empty() &&
                                   !skill_duplicate;
            if (use_skill) {
                argv = skill_result.suggested_argv;
                purpose = "skill:" + skill_result.cause;
                timing.mark_proposal();
                transition(Phase::Probe, Phase::Propose,
                           "skill=" + skill_result.cause + " confidence=" +
                               std::to_string(skill_result.confidence));
            } else {
                transition(Phase::Probe, Phase::Diagnose,
                           skill_duplicate ? "skill proposal was already attempted"
                                           : "skills inconclusive");

                std::vector<Message> history;
                Message system_message;
                system_message.role = "system";
                system_message.content = build_system_prompt(policy_);
                history.push_back(std::move(system_message));

                Message user_message;
                user_message.role = "user";
                user_message.content = build_user_message(line, previous_failure, skill_result);
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
                    if (fail(Phase::Diagnose, "llm_error", reply.error().message, {}, {})) {
                        continue;
                    }
                    return;
                }

                const std::optional<Proposal> proposal = proposal_from_reply(reply.value());
                if (!proposal.has_value()) {
                    if (fail(Phase::Diagnose, "no_tool_call",
                             "LLM returned no usable run_command", {}, {})) {
                        continue;
                    }
                    return;
                }

                argv = proposal->argv;
                purpose = proposal->purpose;
                timing.mark_proposal();
                transition(Phase::Diagnose, Phase::Propose, "proposal " + join_argv(argv));
            }
        }

        if (std::find(attempted_argv.begin(), attempted_argv.end(), argv) !=
            attempted_argv.end()) {
            if (fail(Phase::Propose, "duplicate_proposal",
                     "proposal repeated an attempted argv: " + join_argv(argv), {}, argv)) {
                continue;
            }
            return;
        }
        attempted_argv.push_back(argv);

        transition(Phase::Propose, Phase::Authorize, "validate proposal");
        const PolicyVerdict verdict = policy_.check(argv);
        if (verdict.decision != Decision::Allow) {
            if (fail(Phase::Authorize, "denied",
                     "rule=" + verdict.rule_id + " reason=" + verdict.reason, {}, argv)) {
                continue;
            }
            return;
        }
        transition(Phase::Authorize, Phase::Act, "allow rule=" + verdict.rule_id);

        if (!verdict.resolved_program.empty()) {
            argv[0] = verdict.resolved_program.string();
        }

        verifier_.before_repair(parsed.template_id);
        const ExecResult result = runner_.run(argv);
        const std::string command_detail =
            "exit=" + std::to_string(result.exit_code) +
            (result.timed_out ? " timed_out" : "") +
            (result.err.empty() ? "" : " err=" + result.err.substr(0, 200));
        if (result.exit_code != 0 || result.timed_out) {
            if (fail(Phase::Act, "command_failed", command_detail, {}, argv)) {
                continue;
            }
            return;
        }
        transition(Phase::Act, Phase::Verify, "command " + command_detail);

        const Verdict verification = verifier_.verify(parsed.template_id);
        if (!verification.ok) {
            if (fail(Phase::Verify, "verification_failed", "verification failed",
                     verification.checks, argv)) {
                continue;
            }
            return;
        }
        transition(Phase::Verify, Phase::Learn, "verification passed");

        FixRecord record;
        record.argv = argv;
        record.purpose = purpose;
        record.successes = 1;
        if (recall_hit && hits.front().template_id == parsed.template_id &&
            hits.front().record != nullptr) {
            record.successes = hits.front().record->successes + 1;
            record.failures = hits.front().record->failures;
        }
        record.last_seen = std::chrono::system_clock::now();
        const Result<void> learned =
            memory_.upsert(parsed.template_id, parsed.template_text, std::move(record));
        if (!learned.has_value()) {
            transition(Phase::Learn, Phase::Cooldown,
                       "memory write failed: " + learned.error().message);
            return;
        }

        cooldown_guard.mark_success();
        transition(Phase::Learn, Phase::Done,
                   "stored fix for " + hex_id(parsed.template_id) +
                       " after " + std::to_string(attempt) + " attempt(s)");
        return;
    }

    transition(Phase::Replan, Phase::Cooldown, "attempts exhausted");
}

}  // namespace agenticpp
