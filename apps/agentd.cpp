// agentd — the daemon. Stage 1: argument parsing + deterministic replay.
// Execution and the live tail arrive in later stages.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "agenticpp/line_buffer.hpp"
#include "agenticpp/template_parser.hpp"
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
        "\n"
        "planned:\n"
        "  --config PATH        runtime config (default config/agent.json)\n"
        "  --dry-run            never execute commands (default: on)\n"
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

        std::fprintf(stderr, "agentd: unknown option '%.*s'\n", static_cast<int>(arg.size()),
                     arg.data());
        print_usage(self);
        return 2;
    }

    if (has_replay) {
        return replay_file(replay_path, stats, from_start, dump_templates);
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
