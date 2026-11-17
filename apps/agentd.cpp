// agentd — the daemon. Stage 1: argument parsing + deterministic replay.
// Execution and the live tail arrive in later stages.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agenticpp/agent.hpp"
#include "agenticpp/event_sink.hpp"
#include "agenticpp/executor.hpp"
#include "agenticpp/line_buffer.hpp"
#include "agenticpp/llm_client.hpp"
#include "agenticpp/memory_store.hpp"
#include "agenticpp/policy.hpp"
#include "agenticpp/template_parser.hpp"
#include "agenticpp/verifier.hpp"
#include "agenticpp/version.hpp"

namespace {

void print_usage(std::string_view argv0) {
    std::printf(
        "usage: %.*s [options]\n"
        "\n"
        "  --version            print build info and exit\n"
        "  --help               print this help and exit\n"
        "  --replay PATH        replay a log file once, from byte 0\n"
        "  --from-start         accepted for compatibility; replay starts at byte 0\n"
        "  --stats              print replay line/dropped/byte counts\n"
        "  --dump-templates     print each parsed template id and masked text\n"
        "  --seed-fix TEXT PROGRAM  seed an in-memory fix for a dev probe\n"
        "  --recall TEXT        query the in-memory exact-match store\n"
        "  --check-command ...  check an argv vector against config/agent.json\n"
        "  --dry-run            run the Agent loop on replay (no execution)\n"
        "  --llm=cassette       use a recorded ChatReply for --dry-run\n"
        "  --cassette PATH      cassette file for --dry-run\n"
        "\n"
        "planned:\n"
        "  --config PATH        runtime config (default config/agent.json)\n"
        "  --execute            allow the policy gate to run commands\n"
        "  --yes                with --execute: unattended, no per-command confirmation\n"
        "  --no-llm             deterministic path only\n",
        static_cast<int>(argv0.size()), argv0.data());
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    std::string data;
    data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        return std::nullopt;
    }
    return data;
}

std::filesystem::path project_root() {
    std::filesystem::path root = std::filesystem::current_path();
    if (!std::filesystem::exists(root / "config/agent.json")) {
#ifdef AGENTICPP_SOURCE_DIR
        root = std::filesystem::path{AGENTICPP_SOURCE_DIR};
#endif
    }
    return root;
}

agenticpp::Result<agenticpp::Policy> load_policy() {
    const std::filesystem::path root = project_root();
    return agenticpp::Policy::from_file(root / "config/agent.json", root);
}

class CassetteLlmClient final : public agenticpp::LlmClient {
public:
    explicit CassetteLlmClient(std::string path) : path_(std::move(path)) {}

    agenticpp::Result<agenticpp::ChatReply> chat(std::span<const agenticpp::Message>,
                                                 std::span<const agenticpp::ToolDef>,
                                                 const agenticpp::ChatOptions&) override {
        const auto data = read_file(path_);
        if (!data.has_value()) {
            return agenticpp::Result<agenticpp::ChatReply>::err(
                1, "cannot read cassette: " + path_);
        }

        try {
            const nlohmann::json json = nlohmann::json::parse(*data);
            agenticpp::ChatReply reply;
            reply.content = json.value("content", std::string{});
            reply.finish_reason = json.value("finish_reason", std::string{});

            if (json.contains("tool_calls") && json["tool_calls"].is_array()) {
                for (const auto& item : json["tool_calls"]) {
                    agenticpp::ToolCall call;
                    call.id = item.value("id", std::string{});
                    call.name = item.value("name", std::string{});
                    call.arguments_json = item.value("arguments_json", std::string{});
                    reply.tool_calls.push_back(std::move(call));
                }
            }
            return agenticpp::Result<agenticpp::ChatReply>::ok(std::move(reply));
        } catch (const std::exception& error) {
            return agenticpp::Result<agenticpp::ChatReply>::err(
                2, std::string{"invalid cassette: "} + error.what());
        }
    }

    agenticpp::Result<void> warmup() override { return agenticpp::Result<void>::ok(); }

private:
    std::string path_;
};

int run_agent_replay(const std::string& path, const std::string& cassette_path, bool stats) {
    const auto data = read_file(path);
    if (!data.has_value()) {
        std::fprintf(stderr, "agentd: cannot read '%s'\n", path.c_str());
        return 2;
    }

    auto policy = load_policy();
    if (!policy.has_value()) {
        std::fprintf(stderr, "agentd: policy error: %s\n", policy.error().message.c_str());
        return 2;
    }

    CassetteLlmClient llm(cassette_path);
    agenticpp::MemoryStore memory;
    agenticpp::DryRunRunner runner;
    agenticpp::AlwaysOkVerifier verifier;
    agenticpp::ConsoleSink sink;
    agenticpp::AgentConfig config;
    agenticpp::Agent agent(config, memory, llm, policy.value(), runner, verifier, sink);

    agenticpp::LineBuffer buffer;
    buffer.append(std::span<const char>(data->data(), data->size()));
    buffer.flush_eof();

    std::size_t lines = 0;
    while (const auto line = buffer.next_line()) {
        ++lines;
        agent.on_line(*line);
        agent.run_once();
    }

    if (stats) {
        std::printf("replay: lines=%zu dropped=%zu bytes=%zu\n", lines, buffer.dropped_lines(),
                    data->size());
    }
    return 0;
}

int memory_demo(bool has_seed, const std::string& seed_text, const std::string& seed_program,
                bool has_recall, const std::string& recall_query) {
    agenticpp::MemoryStore store;

    if (has_seed) {
        agenticpp::FixRecord record;
        record.argv.push_back(seed_program);
        record.purpose = "manual seed";
        const std::uint64_t id = agenticpp::fnv1a64(seed_text);
        store.upsert(id, seed_text, record);
        std::printf("seeded id=%016llx argv=[%s]\n", static_cast<unsigned long long>(id),
                    seed_program.c_str());
    }

    if (has_recall) {
        const std::vector<agenticpp::Hit> hits = store.search(recall_query);
        std::printf("recall: hits=%zu\n", hits.size());
        for (const agenticpp::Hit& hit : hits) {
            const char* argv0 = "-";
            if (hit.record != nullptr && !hit.record->argv.empty()) {
                argv0 = hit.record->argv.front().c_str();
            }
            std::printf("  score=%.3f id=%016llx argv0=%s purpose=\"%s\"\n",
                        static_cast<double>(hit.score),
                        static_cast<unsigned long long>(hit.template_id), argv0,
                        hit.record != nullptr ? hit.record->purpose.c_str() : "");
        }
    }

    return 0;
}

int replay_file(const std::string& path, bool stats, bool from_start, bool dump_templates) {
    // Stage 1 has no offset tracking. Replay always starts at byte 0; --from-start is
    // accepted now so scripts and tests do not have to change when the live source lands.
    (void)from_start;

    const auto data = read_file(path);
    if (!data.has_value()) {
        std::fprintf(stderr, "agentd: cannot read '%s'\n", path.c_str());
        return 2;
    }

    agenticpp::LineBuffer buffer;
    buffer.append(std::span<const char>(data->data(), data->size()));
    buffer.flush_eof();

    std::size_t lines = 0;
    while (const auto line = buffer.next_line()) {
        ++lines;
        if (dump_templates) {
            const agenticpp::ParsedLine parsed = agenticpp::parse_template(*line);
            const std::size_t display_size = std::min<std::size_t>(parsed.template_text.size(), 200);
            std::printf("template=%016llx params=%zu text=\"%.*s\"\n",
                        static_cast<unsigned long long>(parsed.template_id), parsed.params.size(),
                        static_cast<int>(display_size), parsed.template_text.data());
        } else if (!stats) {
            if (line->empty()) {
                std::printf("\n");
            } else {
                std::printf("%.*s\n", static_cast<int>(line->size()), line->data());
            }
        }
    }

    if (stats) {
        std::printf("replay: lines=%zu dropped=%zu bytes=%zu\n", lines, buffer.dropped_lines(),
                    data->size());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view self = (argc > 0 && argv[0] != nullptr) ? argv[0] : "agentd";

    std::string replay_path;
    bool has_replay = false;
    bool stats = false;
    bool from_start = false;
    bool dump_templates = false;
    bool has_seed = false;
    bool has_recall = false;
    std::string seed_text;
    std::string seed_program;
    std::string recall_query;
    bool has_check_command = false;
    std::vector<std::string> check_command;
    bool dry_run = false;
    std::string llm_mode;
    std::string cassette_path;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--version") {
            std::printf("%.*s\n", static_cast<int>(agenticpp::build_info().size()),
                        agenticpp::build_info().data());
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(self);
            return 0;
        }
        if (arg == "--replay") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentd: --replay requires a path\n");
                return 2;
            }
            replay_path = argv[++i];
            has_replay = true;
            continue;
        }
        if (arg == "--stats") {
            stats = true;
            continue;
        }
        if (arg == "--from-start") {
            from_start = true;
            continue;
        }
        if (arg == "--dump-templates") {
            dump_templates = true;
            continue;
        }
        if (arg == "--seed-fix") {
            if (i + 2 >= argc) {
                std::fprintf(stderr, "agentd: --seed-fix requires TEXT and PROGRAM\n");
                return 2;
            }
            seed_text = argv[++i];
            seed_program = argv[++i];
            has_seed = true;
            continue;
        }
        if (arg == "--recall") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentd: --recall requires TEXT\n");
                return 2;
            }
            recall_query = argv[++i];
            has_recall = true;
            continue;
        }
        if (arg == "--check-command") {
            has_check_command = true;
            int start = i + 1;
            if (start < argc && std::string_view{argv[start]} == "--") {
                ++start;
            }
            for (int j = start; j < argc; ++j) {
                check_command.emplace_back(argv[j]);
            }
            break;
        }
        if (arg == "--dry-run") {
            dry_run = true;
            continue;
        }
        if (arg == "--llm=cassette") {
            llm_mode = "cassette";
            continue;
        }
        if (arg == "--llm") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentd: --llm requires a mode\n");
                return 2;
            }
            llm_mode = argv[++i];
            continue;
        }
        if (arg == "--cassette") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentd: --cassette requires a path\n");
                return 2;
            }
            cassette_path = argv[++i];
            continue;
        }
        if (!arg.starts_with("--") && llm_mode == "cassette" && cassette_path.empty()) {
            cassette_path = arg;
            continue;
        }

        std::fprintf(stderr, "agentd: unknown option '%.*s'\n", static_cast<int>(arg.size()),
                     arg.data());
        print_usage(self);
        return 2;
    }

    if (has_check_command) {
        if (check_command.empty()) {
            std::fprintf(stderr, "agentd: --check-command requires an argv vector\n");
            return 2;
        }

        auto policy = load_policy();
        if (!policy.has_value()) {
            std::fprintf(stderr, "agentd: policy error: %s\n", policy.error().message.c_str());
            return 2;
        }

        const agenticpp::PolicyVerdict verdict = policy.value().check(check_command);
        const bool allowed = verdict.decision == agenticpp::Decision::Allow;
        std::printf("%s rule=%s reason=%s\n", allowed ? "ALLOW" : "DENY",
                    verdict.rule_id.c_str(), verdict.reason.c_str());
        return allowed ? 0 : 1;
    }

    if (has_seed || has_recall) {
        const int result = memory_demo(has_seed, seed_text, seed_program, has_recall, recall_query);
        if (result != 0 || !has_replay) {
            return result;
        }
    }

    if (has_replay) {
        if (dry_run) {
            if (llm_mode != "cassette" || cassette_path.empty()) {
                std::fprintf(stderr, "agentd: --dry-run requires --llm=cassette PATH\n");
                return 2;
            }
            return run_agent_replay(replay_path, cassette_path, stats);
        }
        return replay_file(replay_path, stats, from_start, dump_templates);
    }

    if (dry_run) {
        std::fprintf(stderr, "agentd: --dry-run requires --replay\n");
        return 2;
    }

    if (stats || dump_templates) {
        std::fprintf(stderr, "agentd: --stats and --dump-templates require --replay\n");
        return 2;
    }

    std::printf("%.*s\n", static_cast<int>(agenticpp::build_info().size()),
                agenticpp::build_info().data());
    std::fprintf(stderr, "agentd: not implemented yet (see docs/WORKER_PLAN.md)\n");
    return 1;
}
