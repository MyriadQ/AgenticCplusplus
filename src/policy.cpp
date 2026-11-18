#include "agenticpp/policy.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace agenticpp {
namespace {

namespace fs = std::filesystem;

PolicyVerdict deny(std::string rule_id, std::string reason) {
    return PolicyVerdict{Decision::Deny, std::move(rule_id), std::move(reason), {}};
}

bool is_within(const fs::path& path, const fs::path& root) {
    std::error_code error;
    const fs::path relative = fs::relative(path, root, error);
    if (error || relative.empty()) {
        return false;
    }
    return relative.native().rfind("..", 0) != 0;
}

fs::path resolve(const fs::path& path, std::error_code& error) {
    return fs::weakly_canonical(path, error);
}

}  // namespace

Result<Policy> Policy::from_file(const fs::path& config_path, const fs::path& root) {
    std::ifstream input(config_path);
    if (!input) {
        return Result<Policy>::err(1, "cannot open policy config: " + config_path.string());
    }

    nlohmann::json config;
    try {
        input >> config;
    } catch (const std::exception& error) {
        return Result<Policy>::err(2, std::string{"invalid policy JSON: "} + error.what());
    }

    if (!config.is_object() || !config.contains("allow") || !config["allow"].is_array()) {
        return Result<Policy>::err(3, "policy config must contain an allow array");
    }

    Policy policy;

    std::error_code error;
    fs::path canonical_root = fs::weakly_canonical(root, error);
    if (error) {
        error.clear();
        canonical_root = fs::absolute(root, error);
        if (error) {
            return Result<Policy>::err(4, "cannot resolve project root: " + root.string());
        }
    }
    policy.root_ = std::move(canonical_root);

    for (const auto& item : config["allow"]) {
        if (!item.is_object()) {
            return Result<Policy>::err(5, "each allow entry must be an object");
        }

        Rule rule;
        rule.id = item.value("id", std::string{});
        rule.program = item.value("program", std::string{});
        rule.doc = item.value("doc", std::string{});

        const int max_args = item.value("max_args", 0);
        if (rule.id.empty() || rule.program.empty() || max_args < 0) {
            return Result<Policy>::err(6, "allow entry needs id, program, and max_args >= 0");
        }
        rule.max_args = static_cast<std::size_t>(max_args);

        fs::path program_path{rule.program};
        if (!program_path.is_absolute()) {
            program_path = policy.root_ / program_path;
        }

        error.clear();
        const fs::path resolved_program = resolve(program_path, error);
        if (error) {
            return Result<Policy>::err(7, "cannot resolve allow entry: " + rule.program);
        }
        if (!is_within(resolved_program, policy.root_)) {
            return Result<Policy>::err(8, "allow entry escapes project root: " + rule.program);
        }

        policy.resolved_programs_.push_back(resolved_program);
        policy.rules_.push_back(std::move(rule));
    }

    if (config.contains("deny_programs")) {
        if (!config["deny_programs"].is_array()) {
            return Result<Policy>::err(9, "deny_programs must be an array");
        }
        for (const auto& item : config["deny_programs"]) {
            if (!item.is_string()) {
                return Result<Policy>::err(10, "deny_programs entries must be strings");
            }
            policy.deny_programs_.push_back(item.get<std::string>());
        }
    }

    return Result<Policy>::ok(std::move(policy));
}

PolicyVerdict Policy::check(const std::vector<std::string>& argv) const {
    if (argv.empty()) {
        return deny("", "empty argv");
    }

    for (const std::string& arg : argv) {
        if (arg.find('\0') != std::string::npos) {
            return deny("", "NUL byte in argument");
        }
        if (arg.find("..") != std::string::npos) {
            return deny("", "path traversal in argument");
        }
        if (arg.size() > 256) {
            return deny("", "argument too long");
        }
    }

    const std::string& program = argv[0];
    if (program.empty()) {
        return deny("", "empty program");
    }

    const std::string base = std::filesystem::path(program).filename().string();
    for (const std::string& denied : deny_programs_) {
        if (base == denied) {
            return deny("", "program is on deny list: " + denied);
        }
    }

    if (program.find('/') == std::string::npos) {
        return deny("", "program must be an explicit path");
    }

    std::filesystem::path candidate{program};
    if (!candidate.is_absolute()) {
        candidate = root_ / candidate;
    }

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(candidate, error);
    if (!error && std::filesystem::is_symlink(status)) {
        return deny("", "program path is a symlink");
    }

    error.clear();
    const std::filesystem::path resolved_program = resolve(candidate, error);
    if (error) {
        return deny("", "cannot resolve program path: " + program);
    }
    if (!is_within(resolved_program, root_)) {
        return deny("", "program is outside the project root: " + program);
    }

    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg.empty() || arg.front() != '/') {
            continue;
        }
        error.clear();
        const std::filesystem::path resolved_arg = resolve(arg, error);
        if (error || !is_within(resolved_arg, root_ / "data")) {
            return deny("", "absolute argument is outside data/: " + arg);
        }
    }

    for (std::size_t i = 0; i < rules_.size(); ++i) {
        if (resolved_programs_[i] != resolved_program) {
            continue;
        }
        if (argv.size() - 1 > rules_[i].max_args) {
            return deny(rules_[i].id, "too many arguments");
        }
        return PolicyVerdict{Decision::Allow, rules_[i].id, "resolved allowlist match",
                              resolved_program};
    }

    return deny("", "program not allowlisted");
}

const std::vector<Rule>& Policy::rules() const noexcept {
    return rules_;
}

}  // namespace agenticpp
