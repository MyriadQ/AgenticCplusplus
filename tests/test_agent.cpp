#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agenticpp/agent.hpp"
#include "agenticpp/executor.hpp"
#include "agenticpp/memory_store.hpp"
#include "agenticpp/policy.hpp"
#include "agenticpp/template_parser.hpp"
#include "agenticpp/verifier.hpp"
#include "fake_llm.hpp"
#include "recording_sink.hpp"

namespace {

class RecordingRunner final : public agenticpp::CommandRunner {
public:
    std::size_t calls = 0;

    agenticpp::ExecResult run(const std::vector<std::string>&) override {
        ++calls;
        agenticpp::ExecResult result;
        result.exit_code = 0;
        return result;
    }
};

class FailingVerifier final : public agenticpp::Verifier {
public:
    agenticpp::Verdict verify(std::uint64_t) override {
        agenticpp::Verdict verdict;
        verdict.ok = false;
        verdict.checks.push_back(agenticpp::Check{"fake_health", false, "always fails"});
        return verdict;
    }
};

std::filesystem::path source_root() {
    return std::filesystem::path{AGENTICPP_SOURCE_DIR};
}

agenticpp::Result<agenticpp::Policy> load_policy() {
    return agenticpp::Policy::from_file(source_root() / "config/agent.json", source_root());
}

agenticpp::ChatReply make_reply(std::vector<std::string> argv, std::string purpose) {
    nlohmann::json arguments;
    arguments["argv"] = argv;
    arguments["purpose"] = purpose;

    agenticpp::ChatReply reply;
    reply.finish_reason = "tool_calls";

    agenticpp::ToolCall call;
    call.id = "call_1";
    call.name = "run_command";
    call.arguments_json = arguments.dump();
    reply.tool_calls.push_back(std::move(call));
    return reply;
}

const std::string kIncident = "2026-01-01T00:00:00Z ERROR bind: address already in use";

std::vector<agenticpp::Phase> visited_phases(const agenticpp::Agent& agent) {
    std::vector<agenticpp::Phase> phases;
    for (const agenticpp::Transition& transition : agent.trail()) {
        phases.push_back(transition.to);
    }
    return phases;
}

}  // namespace

TEST_CASE("Agent: miss calls LLM and learns") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    agenticpp::MemoryStore memory;
    agenticpp::FakeLlmClient llm;
    llm.reply = make_reply({"scripts/kill-decoy.sh"}, "free port 8080");
    agenticpp::DryRunRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;

    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line(kIncident);
    agent.run_once();

    REQUIRE(llm.calls == 1);
    REQUIRE(memory.size() == 1);

    const auto parsed = agenticpp::parse_template(kIncident);
    const auto hits = memory.search(parsed.template_text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().record->argv.front().ends_with("scripts/kill-decoy.sh"));
    REQUIRE_FALSE(sink.events.empty());
}

TEST_CASE("Agent: recall hit skips the LLM") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    const auto parsed = agenticpp::parse_template(kIncident);
    agenticpp::MemoryStore memory;
    agenticpp::FixRecord known;
    known.argv = {"scripts/kill-decoy.sh"};
    known.purpose = "known fix";
    memory.upsert(parsed.template_id, parsed.template_text, known);

    agenticpp::FakeLlmClient llm;
    agenticpp::DryRunRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;

    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line(kIncident);
    agent.run_once();

    REQUIRE(llm.calls == 0);
    REQUIRE(memory.size() == 1);

    const auto phases = visited_phases(agent);
    REQUIRE(std::find(phases.begin(), phases.end(), agenticpp::Phase::Diagnose) == phases.end());
}

TEST_CASE("Agent: low-score recall is treated as a miss") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    agenticpp::MemoryStore memory;
    agenticpp::FixRecord unrelated;
    unrelated.argv = {"scripts/fix-config.sh"};
    unrelated.purpose = "unrelated known fix";
    memory.upsert(agenticpp::fnv1a64("parse config: unexpected token"),
                  "parse config: unexpected token", unrelated);

    agenticpp::FakeLlmClient llm;
    llm.reply = make_reply({"scripts/kill-decoy.sh"}, "free port 8080");
    agenticpp::DryRunRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;

    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line(kIncident);
    agent.run_once();

    REQUIRE(llm.calls == 1);
    REQUIRE(memory.size() == 2);
    REQUIRE(agent.trail().front().to == agenticpp::Phase::Detect);
}

TEST_CASE("Agent: denied argv never reaches the runner") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    agenticpp::MemoryStore memory;
    agenticpp::FakeLlmClient llm;
    llm.reply = make_reply({"sh", "-c", "rm -rf /"}, "dangerous");
    RecordingRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;

    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line(kIncident);
    agent.run_once();

    REQUIRE(llm.calls == 1);
    REQUIRE(runner.calls == 0);
    REQUIRE(memory.size() == 0);
    REQUIRE(agent.trail().back().to == agenticpp::Phase::Cooldown);
}

TEST_CASE("Agent: verifier failure does not learn") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    agenticpp::MemoryStore memory;
    agenticpp::FakeLlmClient llm;
    llm.reply = make_reply({"scripts/kill-decoy.sh"}, "free port 8080");
    agenticpp::DryRunRunner runner;
    FailingVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;

    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line(kIncident);
    agent.run_once();

    REQUIRE(memory.size() == 0);
    REQUIRE(agent.trail().back().to == agenticpp::Phase::Cooldown);
}

TEST_CASE("Agent: phase trail is exact on a successful miss") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    agenticpp::MemoryStore memory;
    agenticpp::FakeLlmClient llm;
    llm.reply = make_reply({"scripts/kill-decoy.sh"}, "free port 8080");
    agenticpp::DryRunRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;

    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line(kIncident);
    agent.run_once();

    const std::vector<agenticpp::Phase> expected{
        agenticpp::Phase::Detect,   agenticpp::Phase::Recall,  agenticpp::Phase::Diagnose,
        agenticpp::Phase::Propose,  agenticpp::Phase::Authorize, agenticpp::Phase::Act,
        agenticpp::Phase::Verify,   agenticpp::Phase::Learn,   agenticpp::Phase::Done,
    };

    REQUIRE(visited_phases(agent) == expected);
}

TEST_CASE("Agent: non-actionable line is ignored") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    agenticpp::MemoryStore memory;
    agenticpp::FakeLlmClient llm;
    agenticpp::DryRunRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;

    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line("level=info service=victim tick=1 port=8080");
    agent.run_once();

    REQUIRE(llm.calls == 0);
    REQUIRE(memory.size() == 0);
    REQUIRE(agent.trail().empty());
}

TEST_CASE("Agent: persisted journal gives a recall hit after restart") {
    auto policy = load_policy();
    REQUIRE(policy.has_value());

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "agenticpp_agent_persist";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);

    agenticpp::MemoryStoreConfig memory_config;
    memory_config.snapshot_path = root / "memory.bin";
    memory_config.journal_path = root / "memory.journal";
    memory_config.persistent = true;
    memory_config.write_through = true;
    memory_config.compact_after_events = 0;

    {
        agenticpp::MemoryStore memory{agenticpp::Vectorizer{}, memory_config};
        agenticpp::FakeLlmClient llm;
        llm.reply = make_reply({"scripts/kill-decoy.sh"}, "free port 8080");
        agenticpp::DryRunRunner runner;
        agenticpp::AlwaysOkVerifier verifier;
        agenticpp::RecordingSink sink;
        agenticpp::AgentConfig config;

        agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
        agent.on_line(kIncident);
        agent.run_once();

        REQUIRE(llm.calls == 1);
        REQUIRE(memory.size() == 1);
        REQUIRE(agent.trail().back().to == agenticpp::Phase::Done);
    }

    agenticpp::MemoryStore memory{agenticpp::Vectorizer{}, memory_config};
    REQUIRE(memory.load().has_value());
    REQUIRE(memory.size() == 1);

    agenticpp::FakeLlmClient llm;
    agenticpp::DryRunRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::RecordingSink sink;
    agenticpp::AgentConfig config;
    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);
    agent.on_line(kIncident);
    agent.run_once();

    REQUIRE(llm.calls == 0);
    const auto phases = visited_phases(agent);
    REQUIRE(std::find(phases.begin(), phases.end(), agenticpp::Phase::Diagnose) == phases.end());
    REQUIRE(agent.trail().back().to == agenticpp::Phase::Done);

    std::filesystem::remove_all(root, error);
}
