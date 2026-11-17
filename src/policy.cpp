#include "agenticpp/policy.hpp"

#include <fstream>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace agenticpp {
namespace {

PolicyVerdict deny(std::string rule_id, std::string reason) {
    return PolicyVerdict{Decision::Deny, std::move(rule_id), std::move(reason)};
}

}  // namespace

Result<Policy> Policy::from_file(const std::filesystem::path& config_path,
                                 const std::filesystem::path& root) {
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
    policy.root_ = root;

    for (const auto& item : config["allow"]) {
        if (!item.is_object()) {
            return Result<Policy>::err(4, "each allow entry must be an object");
        }

        Rule rule;
        rule.id = item.value("id", std::string{});
        rule.program = item.value("program", std::string{});
        rule.doc = item.value("doc", std::string{});

        const int max_args = item.value("max_args", 0);
        if (rule.id.empty() || rule.program.empty() || max_args < 0) {
            return Result<Policy>::err(5, "allow entry needs id, program, and max_args >= 0");
        }
        rule.max_args = static_cast<std::size_t>(max_args);
        policy.rules_.push_back(std::move(rule));
    }

    if (config.contains("deny_programs")) {
        if (!config["deny_programs"].is_array()) {
            return Result<Policy>::err(6, "deny_programs must be an array");
        }
        for (const auto& item : config["deny_programs"]) {
            if (!item.is_string()) {
                return Result<Policy>::err(7, "deny_programs entries must be strings");
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

    for (const Rule& rule : rules_) {
        if (rule.program != program) {
            continue;
        }
        if (argv.size() - 1 > rule.max_args) {
            return deny(rule.id, "too many arguments");
        }
        return PolicyVerdict{Decision::Allow, rule.id, "exact allowlist match"};
    }

    return deny("", "program not allowlisted");
}

const std::vector<Rule>& Policy::rules() const noexcept {
    return rules_;
}

}  // namespace agenticpp
