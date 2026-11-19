// agentd — replay-driven repair agent with a bounded phase machine.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agenticpp/agent.hpp"
#include "agenticpp/config.hpp"
#include "agenticpp/event_sink.hpp"
#include "agenticpp/executor.hpp"
#include "agenticpp/incident_queue.hpp"
#include "agenticpp/line_buffer.hpp"
#include "agenticpp/log_source.hpp"
#include "agenticpp/llm_client.hpp"
#include "agenticpp/memory_store.hpp"
#include "agenticpp/policy.hpp"
#include "agenticpp/skills.hpp"
#include "agenticpp/template_parser.hpp"
#include "agenticpp/verifier.hpp"
#include "agenticpp/version.hpp"

#if AGENTICPP_ENABLE_LLM
#include "agenticpp/deepseek_client.hpp"
#endif

namespace {

namespace fs = std::filesystem;

void print_usage(std::string_view argv0) {
    std::printf(
        "usage: %.*s [options]\n"
        "\n"
        "  --version                 print build info and exit\n"
        "  --help                    print this help and exit\n"
        "  --config PATH             runtime config (default config/agent.json)\n"
        "  --replay PATH             replay a log file once, from byte 0\n"
        "  --follow [PATH]           tail the configured log file continuously\n"
        "  --poll-ms N               follow poll interval override\n"
        "  --from-start              follow from byte 0 instead of EOF\n"
        "  --stats                   print replay line/dropped/byte counts\n"
        "  --dump-templates          print each parsed template id and masked text\n"
        "  --dry-run                 run the Agent loop without execution (default for Agent mode)\n"
        "  --execute                 allow the policy gate to run commands (requires --yes)\n"
        "  --yes                     unattended confirmation for --execute\n"
        "  --no-llm                  deterministic recall only\n"
        "  --llm=cassette            use a recorded ChatReply\n"
        "  --cassette PATH           cassette file for --llm=cassette\n"
        "  --llm=deepseek            use the real DeepSeek transport\n"
        "  --llm-smoke               warm up the configured real LLM and exit\n"
        "  --workers N               process incidents with N bounded workers\n"
        "                            (default: guards.max_concurrent_incidents)\n"
        "  --no-skills               force the LLM path instead of local Probe skills\n"
        "  --keep-memory-journal     do not compact the memory journal on exit\n"
        "  --seed-fix TEXT PROGRAM   seed an in-memory fix for a dev probe\n"
        "  --recall TEXT             query the in-memory store\n"
        "  --check-command ...       check an argv vector against the allowlist\n",
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

fs::path project_root() {
    fs::path root = fs::current_path();
    if (!fs::exists(root / "config/agent.json")) {
#ifdef AGENTICPP_SOURCE_DIR
        root = fs::path{AGENTICPP_SOURCE_DIR};
#endif
    }
    return root;
}

fs::path resolve_config_path(const std::string& path) {
    const fs::path candidate{path};
    if (candidate.is_absolute()) {
        return candidate;
    }
    return project_root() / candidate;
}

agenticpp::Result<agenticpp::Policy> load_policy(const std::string& config_path) {
    const fs::path root = project_root();
    return agenticpp::Policy::from_file(resolve_config_path(config_path), root);
}

class CassetteLlmClient final : public agenticpp::LlmClient {
public:
    explicit CassetteLlmClient(std::string path) : path_(std::move(path)) {}

    agenticpp::Result<agenticpp::ChatReply> chat(std::span<const agenticpp::Message>,
                                                  std::span<const agenticpp::ToolDef>,
                                                  const agenticpp::ChatOptions&) override {
        const auto data = read_file(path_);
        if (!data.has_value()) {
            return agenticpp::Result<agenticpp::ChatReply>::err(1,
                                                                "cannot read cassette: " + path_);
        }

        try {
            nlohmann::json json = nlohmann::json::parse(*data);
            if (json.is_object() && json.contains("replies") && json["replies"].is_array()) {
                const auto& replies = json["replies"];
                if (replies.empty()) {
                    return agenticpp::Result<agenticpp::ChatReply>::err(
                        3, "cassette has an empty replies array");
                }
                const std::size_t index = std::min(calls_, replies.size() - 1);
                json = replies[index];
            }
            ++calls_;
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
    std::size_t calls_ = 0;
};

class NoLlmClient final : public agenticpp::LlmClient {
public:
    agenticpp::Result<agenticpp::ChatReply> chat(std::span<const agenticpp::Message>,
                                                  std::span<const agenticpp::ToolDef>,
                                                  const agenticpp::ChatOptions&) override {
        return agenticpp::Result<agenticpp::ChatReply>::err(503, "LLM is disabled");
    }

    agenticpp::Result<void> warmup() override { return agenticpp::Result<void>::ok(); }
};

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

int replay_file(const std::string& path, bool stats, bool dump_templates) {
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

fs::path resolve_project_path(const std::string& path) {
    const fs::path candidate{path};
    if (candidate.is_absolute()) {
        return candidate;
    }
    return project_root() / candidate;
}

int port_from_http_url(std::string_view url, int fallback) {
    const std::size_t colon = url.rfind(':');
    if (colon == std::string_view::npos || colon + 1 >= url.size()) {
        return fallback;
    }
    std::size_t end = colon + 1;
    while (end < url.size() && url[end] >= '0' && url[end] <= '9') {
        ++end;
    }
    if (end == colon + 1) {
        return fallback;
    }
    const int port = std::atoi(std::string{url.substr(colon + 1, end - colon - 1)}.c_str());
    return port > 0 && port <= 65535 ? port : fallback;
}

agenticpp::MemoryStoreConfig memory_config_from(const agenticpp::MemoryConfig& config,
                                               bool write_through) {
    agenticpp::MemoryStoreConfig memory;
    memory.persistent = !config.path.empty();
    memory.snapshot_path = resolve_project_path(config.path);
    if (!config.journal_path.empty()) {
        memory.journal_path = resolve_project_path(config.journal_path);
    } else if (memory.persistent) {
        memory.journal_path = memory.snapshot_path;
        memory.journal_path.replace_extension(".journal");
    }
    memory.write_through = write_through;
    memory.compact_after_events = config.compact_after_events;
    return memory;
}

struct IngestStats {
    std::size_t lines = 0;
    std::size_t dropped = 0;
    std::size_t bytes = 0;
};

using IngestFunction = std::function<IngestStats(agenticpp::IncidentQueue&)>;

volatile std::sig_atomic_t g_follow_stop = 0;

extern "C" void handle_follow_signal(int) {
    g_follow_stop = 1;
}

int run_agent_ingest(const std::string& config_path, bool execute, bool stats,
                     const std::string& llm_mode, const std::string& cassette_path,
                     std::size_t workers, bool compact_on_exit, bool enable_skills,
                     std::string_view stats_label, const std::string& log_path_override,
                     const IngestFunction& ingest) {
    if (execute && fs::exists(project_root() / "data/STOP")) {
        std::fprintf(stderr, "agentd: data/STOP exists; refusing to execute\n");
        return 2;
    }

    auto config = agenticpp::load_config(resolve_config_path(config_path));
    if (!config.has_value()) {
        std::fprintf(stderr, "agentd: config error: %s\n", config.error().message.c_str());
        return 2;
    }

    auto policy = load_policy(config_path);
    if (!policy.has_value()) {
        std::fprintf(stderr, "agentd: policy error: %s\n", policy.error().message.c_str());
        return 2;
    }

    if (workers == 0) {
        workers = std::max<std::size_t>(1, config.value().agent.max_concurrent_incidents);
    }

    if (llm_mode == "cassette" && cassette_path.empty()) {
        std::fprintf(stderr, "agentd: cassette mode requires a path\n");
        return 2;
    }
    if (llm_mode != "cassette" && llm_mode != "none" && llm_mode != "deepseek") {
        std::fprintf(stderr, "agentd: unknown LLM mode '%s'\n", llm_mode.c_str());
        return 2;
    }
#if !AGENTICPP_ENABLE_LLM
    if (llm_mode == "deepseek") {
        std::fprintf(stderr, "agentd: LLM support was not built (AGENTICPP_ENABLE_LLM=0)\n");
        return 2;
    }
#endif

    std::vector<std::unique_ptr<agenticpp::LlmClient>> llms;
    llms.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        if (llm_mode == "cassette") {
            llms.push_back(std::make_unique<CassetteLlmClient>(cassette_path));
            continue;
        }
        if (llm_mode == "none") {
            llms.push_back(std::make_unique<NoLlmClient>());
            continue;
        }
#if AGENTICPP_ENABLE_LLM
        agenticpp::LlmConfig llm_config = config.value().llm;
        if (worker != 0) {
            llm_config.warmup = false;
        }
        auto client = std::make_unique<agenticpp::DeepSeekClient>(llm_config);
        if (worker == 0) {
            auto warm = client->warmup();
            if (!warm.has_value()) {
                std::fprintf(stderr, "agentd: LLM warmup failed: %s\n",
                             warm.error().message.c_str());
                return 2;
            }
            std::printf("agentd: llm warmup ok\n");
        }
        llms.push_back(std::move(client));
#endif
    }

    config.value().executor.sandbox_root = project_root();
    const std::string log_path =
        log_path_override.empty() ? config.value().log_source.path : log_path_override;
    if (!log_path.empty()) {
        config.value().verifier.log_path = resolve_project_path(log_path);
    } else {
        config.value().verifier.log_path.clear();
    }

    std::unique_ptr<agenticpp::CommandRunner> runner;
    if (execute) {
        runner = std::make_unique<agenticpp::ChildProcessRunner>(config.value().executor);
    } else {
        runner = std::make_unique<agenticpp::DryRunRunner>();
    }

    std::vector<std::unique_ptr<agenticpp::Verifier>> verifiers;
    verifiers.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        if (!execute) {
            verifiers.push_back(std::make_unique<agenticpp::AlwaysOkVerifier>());
            continue;
        }
        auto health = std::make_unique<agenticpp::HealthProbeVerifier>(config.value().verifier);
        if (!config.value().verifier.log_path.empty() &&
            config.value().verifier.log_quiet_window.count() > 0) {
            verifiers.push_back(std::make_unique<agenticpp::LogQuietVerifier>(
                std::move(health), config.value().verifier.log_path,
                config.value().verifier.log_quiet_window));
        } else {
            verifiers.push_back(std::move(health));
        }
    }

    agenticpp::CooldownTable cooldown(config.value().agent);
    agenticpp::SkillConfig skill_config;
    skill_config.project_root = project_root();
    skill_config.log_path = config.value().verifier.log_path;
    skill_config.run_dir = project_root() / "run";
    skill_config.default_port =
        port_from_http_url(config.value().verifier.health_url, 18080);
    agenticpp::SkillRegistry skills{std::move(skill_config)};

    agenticpp::ConsoleSink console;
    agenticpp::JsonlSink jsonl(project_root() / "data/audit.log");
    agenticpp::TeeSink tee(console, jsonl);
    agenticpp::SynchronizedSink sink(tee);

    agenticpp::MemoryStore memory{agenticpp::Vectorizer{},
                                  memory_config_from(config.value().memory, false)};
    memory.set_write_through(false);
    const auto loaded = memory.load();
    if (!loaded.has_value()) {
        std::fprintf(stderr, "agentd: memory load warning: %s\n",
                     loaded.error().message.c_str());
        if (execute) {
            return 2;
        }
    } else {
        memory.set_write_through(execute);
    }

    std::vector<std::unique_ptr<agenticpp::Agent>> agents;
    agents.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        agents.push_back(std::make_unique<agenticpp::Agent>(
            config.value().agent, memory, *llms[worker], policy.value(), *runner,
            *verifiers[worker], sink, &cooldown, enable_skills ? &skills : nullptr));
    }

    const std::size_t capacity = std::max<std::size_t>(workers * 8, 32);
    agenticpp::IncidentQueue queue(
        workers, capacity,
        [&agents](std::size_t worker, const std::string& line) {
            agents[worker]->on_line(line);
            agents[worker]->run_once();
        });

    const IngestStats totals = ingest(queue);
    queue.wait_idle();
    queue.stop();

    if (execute && loaded.has_value() && compact_on_exit) {
        const auto compacted = memory.compact();
        if (!compacted.has_value()) {
            std::fprintf(stderr, "agentd: memory compact warning: %s\n",
                         compacted.error().message.c_str());
        }
    }

    if (stats) {
        std::printf("%.*s: lines=%zu dropped=%zu bytes=%zu\n",
                    static_cast<int>(stats_label.size()), stats_label.data(), totals.lines,
                    totals.dropped, totals.bytes);
    }
    return 0;
}

int run_agent_replay(const std::string& replay_path, const std::string& config_path, bool execute,
                     bool stats, const std::string& llm_mode, const std::string& cassette_path,
                     std::size_t workers, bool compact_on_exit, bool enable_skills) {
    auto data = read_file(replay_path);
    if (!data.has_value()) {
        std::fprintf(stderr, "agentd: cannot read '%s'\n", replay_path.c_str());
        return 2;
    }

    return run_agent_ingest(
        config_path, execute, stats, llm_mode, cassette_path, workers, compact_on_exit,
        enable_skills, "replay", {}, [data = std::move(data)](agenticpp::IncidentQueue& queue) {
            IngestStats totals;
            totals.bytes = data->size();
            agenticpp::LineBuffer buffer;
            buffer.append(std::span<const char>(data->data(), data->size()));
            buffer.flush_eof();
            while (const auto line = buffer.next_line()) {
                ++totals.lines;
                if (!queue.submit(std::string{*line})) {
                    break;
                }
            }
            totals.dropped = buffer.dropped_lines();
            return totals;
        });
}

int run_agent_follow(const std::string& follow_path, const std::string& config_path, bool execute,
                     bool stats, const std::string& llm_mode, const std::string& cassette_path,
                     std::size_t workers, bool compact_on_exit, bool enable_skills, bool from_start,
                     int poll_ms) {
    auto config = agenticpp::load_config(resolve_config_path(config_path));
    if (!config.has_value()) {
        std::fprintf(stderr, "agentd: config error: %s\n", config.error().message.c_str());
        return 2;
    }

    const std::string path =
        follow_path.empty() ? config.value().log_source.path : follow_path;
    if (path.empty()) {
        std::fprintf(stderr, "agentd: --follow needs a log path\n");
        return 2;
    }
    const int configured_interval = poll_ms > 0 ? poll_ms : config.value().log_source.poll_interval_ms;
    const std::chrono::milliseconds interval{std::max(1, configured_interval)};
    const bool start_at_beginning = from_start || config.value().log_source.from_start;
    auto source = agenticpp::make_polling_source(resolve_project_path(path), interval,
                                                 start_at_beginning);
    if (!source) {
        std::fprintf(stderr, "agentd: cannot create log source for '%s'\n", path.c_str());
        return 2;
    }

    g_follow_stop = 0;
    std::signal(SIGINT, handle_follow_signal);
    std::signal(SIGTERM, handle_follow_signal);
    std::printf("agentd: following %s poll_ms=%d from_start=%s\n", path.c_str(),
                static_cast<int>(interval.count()), start_at_beginning ? "yes" : "no");
    std::fflush(stdout);

    return run_agent_ingest(
        config_path, execute, stats, llm_mode, cassette_path, workers, compact_on_exit,
        enable_skills, "follow", path, [&source, interval](agenticpp::IncidentQueue& queue) {
            IngestStats totals;
            while (g_follow_stop == 0) {
                if (fs::exists(project_root() / "data/STOP")) {
                    std::fprintf(stderr, "agentd: data/STOP present; stopping follow loop\n");
                    break;
                }
                const auto records = source->poll();
                for (const agenticpp::LogRecord& record : records) {
                    ++totals.lines;
                    if (!queue.submit(std::string{record.line})) {
                        break;
                    }
                }
                if (records.empty()) {
                    source->wait(interval);
                }
            }

            source->flush_eof();
            const auto final_records = source->poll();
            for (const agenticpp::LogRecord& record : final_records) {
                ++totals.lines;
                if (!queue.submit(std::string{record.line})) {
                    break;
                }
            }
            const agenticpp::LogSourceStats source_stats = source->stats();
            totals.dropped = source_stats.dropped;
            totals.bytes = source_stats.bytes;
            return totals;
        });
}

int check_command_argv(const std::vector<std::string>& command, const std::string& config_path) {
    if (command.empty()) {
        std::fprintf(stderr, "agentd: --check-command requires an argv vector\n");
        return 2;
    }

    auto policy = load_policy(config_path);
    if (!policy.has_value()) {
        std::fprintf(stderr, "agentd: policy error: %s\n", policy.error().message.c_str());
        return 2;
    }

    const agenticpp::PolicyVerdict verdict = policy.value().check(command);
    const bool allowed = verdict.decision == agenticpp::Decision::Allow;
    std::printf("%s rule=%s reason=%s\n", allowed ? "ALLOW" : "DENY", verdict.rule_id.c_str(),
                verdict.reason.c_str());
    return allowed ? 0 : 1;
}

int run_llm_smoke(const std::string& config_path) {
#if AGENTICPP_ENABLE_LLM
    auto config = agenticpp::load_config(resolve_config_path(config_path));
    if (!config.has_value()) {
        std::fprintf(stderr, "agentd: config error: %s\n", config.error().message.c_str());
        return 2;
    }

    agenticpp::DeepSeekClient client(config.value().llm);
    auto warm = client.warmup();
    if (!warm.has_value()) {
        std::fprintf(stderr, "agentd: LLM warmup failed: %s\n", warm.error().message.c_str());
        return 1;
    }
    std::printf("agentd: llm warmup ok\n");
    return 0;
#else
    (void)config_path;
    std::fprintf(stderr, "agentd: LLM support was not built (AGENTICPP_ENABLE_LLM=0)\n");
    return 2;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view self = (argc > 0 && argv[0] != nullptr) ? argv[0] : "agentd";

    std::string config_path = "config/agent.json";
    std::string replay_path;
    bool has_replay = false;
    bool has_follow = false;
    bool from_start = false;
    int poll_ms = 0;
    std::string follow_path;
    bool stats = false;
    bool dump_templates = false;
    bool dry_run = false;
    bool execute = false;
    bool yes = false;
    bool llm_smoke = false;
    bool has_seed = false;
    bool has_recall = false;
    std::string seed_text;
    std::string seed_program;
    std::string recall_query;
    bool has_check_command = false;
    std::vector<std::string> check_command;
    std::string llm_mode = "none";
    std::string cassette_path;
    std::size_t workers = 0; // 0 means "use guards.max_concurrent_incidents"
    bool compact_on_exit = true;
    bool enable_skills = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--version") {
            std::printf("%.*s\n", static_cast<int>(agenticpp::build_info().size()),
                        agenticpp::build_info().data());
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(self);
            return 0;
        }
        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentd: --config requires a path\n");
                return 2;
            }
            config_path = argv[++i];
            continue;
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
        if (arg == "--follow") {
            has_follow = true;
            if (i + 1 < argc && !std::string_view{argv[i + 1]}.starts_with("--")) {
                follow_path = argv[++i];
            }
            continue;
        }
        if (arg.starts_with("--follow=")) {
            has_follow = true;
            follow_path = std::string{arg.substr(std::string_view{"--follow="}.size())};
            continue;
        }
        if (arg == "--poll-ms") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentd: --poll-ms requires a value\n");
                return 2;
            }
            char* end = nullptr;
            errno = 0;
            const long value = std::strtol(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || value <= 0) {
                std::fprintf(stderr, "agentd: --poll-ms must be positive\n");
                return 2;
            }
            poll_ms = static_cast<int>(value);
            continue;
        }
        if (arg == "--stats") {
            stats = true;
            continue;
        }
        if (arg == "--dump-templates") {
            dump_templates = true;
            continue;
        }
        if (arg == "--from-start") {
            from_start = true;
            continue;
        }
        if (arg == "--dry-run") {
            dry_run = true;
            continue;
        }
        if (arg == "--execute") {
            execute = true;
            continue;
        }
        if (arg == "--yes") {
            yes = true;
            continue;
        }
        if (arg == "--no-llm") {
            llm_mode = "none";
            continue;
        }
        if (arg == "--llm=cassette") {
            llm_mode = "cassette";
            continue;
        }
        if (arg == "--llm=deepseek") {
            llm_mode = "deepseek";
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
        if (arg == "--llm-smoke") {
            llm_smoke = true;
            continue;
        }
        if (arg == "--keep-memory-journal") {
            compact_on_exit = false;
            continue;
        }
        if (arg == "--no-skills") {
            enable_skills = false;
            continue;
        }
        if (arg == "--workers") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentd: --workers requires a count\n");
                return 2;
            }
            const char* text = argv[++i];
            char* end = nullptr;
            errno = 0;
            const unsigned long long value = std::strtoull(text, &end, 10);
            if (errno != 0 || end == text || *end != '\0' || value == 0) {
                std::fprintf(stderr, "agentd: --workers must be a positive integer\n");
                return 2;
            }
            workers = static_cast<std::size_t>(value);
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
        return check_command_argv(check_command, config_path);
    }

    if (llm_smoke) {
        return run_llm_smoke(config_path);
    }

    if (has_seed || has_recall) {
        if (has_replay || has_follow || dry_run || execute || llm_smoke) {
            std::fprintf(stderr, "agentd: memory probe flags cannot be combined with replay/follow\n");
            return 2;
        }
        return memory_demo(has_seed, seed_text, seed_program, has_recall, recall_query);
    }

    if (has_replay && has_follow) {
        std::fprintf(stderr, "agentd: --replay and --follow are mutually exclusive\n");
        return 2;
    }
    if (has_follow && !dry_run && !execute) {
        dry_run = true;
    }
    if (has_follow && dump_templates) {
        std::fprintf(stderr, "agentd: --dump-templates is only available with --replay\n");
        return 2;
    }
    if (dry_run && execute) {
        std::fprintf(stderr, "agentd: --dry-run and --execute are mutually exclusive\n");
        return 2;
    }
    if (execute && !yes) {
        std::fprintf(stderr, "agentd: --execute requires --yes\n");
        return 2;
    }
    if (yes && !execute) {
        std::fprintf(stderr, "agentd: --yes only makes sense with --execute\n");
        return 2;
    }

    if (has_follow) {
        return run_agent_follow(follow_path, config_path, execute, stats, llm_mode,
                                cassette_path, workers, compact_on_exit, enable_skills, from_start,
                                poll_ms);
    }

    if (has_replay) {
        if (dry_run || execute) {
            return run_agent_replay(replay_path, config_path, execute, stats, llm_mode,
                                    cassette_path, workers, compact_on_exit, enable_skills);
        }
        return replay_file(replay_path, stats, dump_templates);
    }

    if (dry_run || execute) {
        std::fprintf(stderr, "agentd: --dry-run/--execute require --replay or --follow\n");
        return 2;
    }
    if (stats || dump_templates) {
        std::fprintf(stderr, "agentd: --stats and --dump-templates require --replay\n");
        return 2;
    }

    std::printf("%.*s\n", static_cast<int>(agenticpp::build_info().size()),
                agenticpp::build_info().data());
    std::fprintf(stderr, "agentd: no action selected (see --help)\n");
    return 1;
}
