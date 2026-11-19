#include "agenticpp/skills.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <signal.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "agenticpp/line_buffer.hpp"

namespace agenticpp {
namespace {

namespace fs = std::filesystem;

std::string hex_id(std::uint64_t id) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(id));
    return buffer;
}

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

int find_port(const ParsedLine& parsed, int fallback) {
    int found = fallback;
    for (const std::string& value : parsed.params) {
        try {
            std::size_t consumed = 0;
            const int number = std::stoi(value, &consumed, 10);
            if (consumed == value.size() && number > 0 && number <= 65535) {
                found = number;
            }
        } catch (const std::exception&) {
            // A non-numeric parameter is ignored.
        }
    }
    return found;
}

bool port_listening(std::uint16_t port) {
    for (const char* path : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        std::ifstream input(path);
        if (!input) {
            continue;
        }
        std::string text;
        std::getline(input, text); // header
        while (std::getline(input, text)) {
            std::istringstream line{text};
            std::string slot;
            std::string local;
            std::string remote;
            std::string state;
            if (!(line >> slot >> local >> remote >> state)) {
                continue;
            }
            if (state != "0A") { // TCP_LISTEN
                continue;
            }
            const std::size_t colon = local.rfind(':');
            if (colon == std::string::npos) {
                continue;
            }
            try {
                const unsigned long parsed =
                    std::stoul(local.substr(colon + 1), nullptr, 16);
                if (parsed == static_cast<unsigned long>(port)) {
                    return true;
                }
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    return false;
}

std::optional<pid_t> read_pid(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    long value = 0;
    input >> value;
    if (!input || value <= 0) {
        return std::nullopt;
    }
    return static_cast<pid_t>(value);
}

bool process_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    errno = 0;
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

bool is_within(const fs::path& path, const fs::path& root) {
    std::error_code error;
    const fs::path relative = fs::relative(path, root, error);
    if (error || relative.empty()) {
        return false;
    }
    return relative.native().rfind("..", 0) != 0;
}

std::optional<fs::path> config_path_from(const ParsedLine& parsed,
                                         const fs::path& project_root) {
    const fs::path root = project_root.empty() ? fs::current_path() : project_root;
    for (const std::string& value : parsed.params) {
        const fs::path candidate{value};
        const std::string extension = candidate.extension().string();
        if (extension != ".json" && extension != ".yaml" && extension != ".yml" &&
            value.find('/') == std::string::npos) {
            continue;
        }
        std::error_code error;
        const fs::path absolute = candidate.is_absolute() ? candidate : root / candidate;
        const fs::path resolved = fs::weakly_canonical(absolute, error);
        if (error || !is_within(resolved, root)) {
            continue;
        }
        return resolved;
    }
    return std::nullopt;
}

class PortConflictSkill final : public Skill {
public:
    explicit PortConflictSkill(SkillConfig config) : config_(std::move(config)) {}

    SkillResult probe(const SkillContext& context) const override {
        SkillResult result;
        if (context.line.find("bind:") == std::string_view::npos &&
            context.line.find("address already in use") == std::string_view::npos) {
            return result;
        }

        const int port = find_port(context.parsed, config_.default_port);
        result.evidence.push_back(
            Evidence{"port_conflict", "port " + std::to_string(port) +
                                         (port_listening(static_cast<std::uint16_t>(port))
                                              ? " is in LISTEN state"
                                              : " is not listening")});
        if (!port_listening(static_cast<std::uint16_t>(port))) {
            return result;
        }

        const fs::path run = config_.run_dir.empty() ? fs::path{"run"} : config_.run_dir;
        const fs::path decoy_pid_file = run / "decoy.pid";
        const std::optional<pid_t> decoy_pid = read_pid(decoy_pid_file);
        if (decoy_pid.has_value() && process_alive(*decoy_pid)) {
            result.evidence.push_back(Evidence{
                "port_conflict",
                "decoy pid " + std::to_string(*decoy_pid) + " owns the port"});
            result.conclusive = true;
            result.cause = "port_conflict";
            result.confidence = 0.99;
            result.suggested_argv = {"scripts/kill-decoy.sh"};
            return result;
        }

        result.evidence.push_back(Evidence{
            "port_conflict",
            "port is held by an unknown process; no live decoy pidfile"});
        return result;
    }

private:
    SkillConfig config_;
};

class ConfigInvalidSkill final : public Skill {
public:
    explicit ConfigInvalidSkill(SkillConfig config) : config_(std::move(config)) {}

    SkillResult probe(const SkillContext& context) const override {
        SkillResult result;
        if (context.line.find("parse config") == std::string_view::npos &&
            context.line.find("unexpected token") == std::string_view::npos &&
            context.line.find("config") == std::string_view::npos) {
            return result;
        }

        const auto path = config_path_from(context.parsed, config_.project_root);
        if (!path.has_value()) {
            result.evidence.push_back(Evidence{
                "config_invalid", "no project-local config path in the incident"});
            return result;
        }

        std::ifstream input(*path, std::ios::binary);
        if (!input) {
            result.evidence.push_back(Evidence{
                "config_invalid", "cannot read " + path->string()});
            return result;
        }

        std::string text((std::istreambuf_iterator<char>(input)), {});
        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            (void)parsed;
            result.evidence.push_back(Evidence{
                "config_invalid", path->string() + " parses successfully"});
            return result;
        } catch (const std::exception& error) {
            result.evidence.push_back(Evidence{
                "config_invalid",
                path->string() + " failed JSON parse: " + trim(error.what())});
            result.conclusive = true;
            result.cause = "config_invalid";
            result.confidence = 0.95;
            result.suggested_argv = {"scripts/fix-config.sh"};
            return result;
        }
    }

private:
    SkillConfig config_;
};

class ServiceStateSkill final : public Skill {
public:
    explicit ServiceStateSkill(SkillConfig config) : config_(std::move(config)) {}

    SkillResult probe(const SkillContext& context) const override {
        SkillResult result;
        const fs::path run = config_.run_dir.empty() ? fs::path{"run"} : config_.run_dir;
        const fs::path pid_file = run / "victim.pid";
        const std::optional<pid_t> pid = read_pid(pid_file);
        const bool alive = pid.has_value() && process_alive(*pid);
        const int port = find_port(context.parsed, config_.default_port);
        const bool listening = port_listening(static_cast<std::uint16_t>(port));

        result.evidence.push_back(Evidence{
            "service_state",
            "victim pid " + (pid.has_value() ? std::to_string(*pid) : "missing") +
                (alive ? " alive" : " not alive")});
        result.evidence.push_back(Evidence{
            "service_state", "port " + std::to_string(port) +
                                  (listening ? " listening" : " not listening")});

        if (!alive && !listening) {
            result.conclusive = true;
            result.cause = "service_down";
            result.confidence = 0.80;
            result.suggested_argv = {"scripts/restart-victim.sh"};
        }
        return result;
    }

private:
    SkillConfig config_;
};

class RecentOccurrencesSkill final : public Skill {
public:
    explicit RecentOccurrencesSkill(SkillConfig config) : config_(std::move(config)) {}

    SkillResult probe(const SkillContext& context) const override {
        SkillResult result;
        if (config_.log_path.empty()) {
            result.evidence.push_back(
                Evidence{"recent_occurrences", "no log path configured"});
            return result;
        }

        std::ifstream input(config_.log_path, std::ios::binary);
        if (!input) {
            result.evidence.push_back(Evidence{
                "recent_occurrences", "cannot read " + config_.log_path.string()});
            return result;
        }
        input.seekg(0, std::ios::end);
        const std::streamoff size = input.tellg();
        if (size < 0) {
            result.evidence.push_back(
                Evidence{"recent_occurrences", "cannot determine log size"});
            return result;
        }
        const std::size_t cap = config_.recent_log_bytes;
        const std::streamoff start = size > static_cast<std::streamoff>(cap)
                                         ? size - static_cast<std::streamoff>(cap)
                                         : 0;
        input.seekg(start);
        std::string content(static_cast<std::size_t>(size - start), '\0');
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(input.gcount()));

        LineBuffer buffer;
        buffer.append(std::span<const char>(content.data(), content.size()));
        buffer.flush_eof();
        std::size_t occurrences = 0;
        while (const auto line = buffer.next_line()) {
            if (parse_template(*line).template_id == context.parsed.template_id) {
                ++occurrences;
            }
        }

        result.evidence.push_back(Evidence{
            "recent_occurrences",
            "template " + hex_id(context.parsed.template_id) + " appears " +
                std::to_string(occurrences) + " time(s) in the last " +
                std::to_string(content.size()) + " bytes"});
        return result;
    }

private:
    SkillConfig config_;
};

}  // namespace

SkillRegistry::SkillRegistry(SkillConfig config) : config_(std::move(config)) {
    skills_.push_back(std::make_unique<PortConflictSkill>(config_));
    skills_.push_back(std::make_unique<ConfigInvalidSkill>(config_));
    skills_.push_back(std::make_unique<ServiceStateSkill>(config_));
    skills_.push_back(std::make_unique<RecentOccurrencesSkill>(config_));
}

SkillRegistry::SkillRegistry(std::vector<std::unique_ptr<Skill>> skills)
    : skills_(std::move(skills)) {}

SkillResult SkillRegistry::probe(const SkillContext& context) const {
    SkillResult result;
    double best_confidence = -1.0;
    for (const std::unique_ptr<Skill>& skill : skills_) {
        SkillResult finding = skill->probe(context);
        result.evidence.insert(result.evidence.end(), finding.evidence.begin(),
                               finding.evidence.end());
        if (!finding.conclusive || finding.suggested_argv.empty()) {
            continue;
        }
        if (finding.confidence > best_confidence) {
            best_confidence = finding.confidence;
            result.conclusive = true;
            result.cause = finding.cause;
            result.confidence = finding.confidence;
            result.suggested_argv = finding.suggested_argv;
        }
    }
    return result;
}

}  // namespace agenticpp
