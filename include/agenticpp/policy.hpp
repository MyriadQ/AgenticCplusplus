#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "agenticpp/result.hpp"
#include "agenticpp/types.hpp"

namespace agenticpp {

enum class Decision {
    Allow,
    Deny,
};

struct PolicyVerdict {
    Decision decision = Decision::Deny;
    std::string rule_id;
    std::string reason;
    std::filesystem::path resolved_program;
};

class Policy {
public:
    static Result<Policy> from_file(const std::filesystem::path& config_path,
                                    const std::filesystem::path& root);

    // Resolves the program path and rejects symlinks before returning Allow.
    // Residual TOCTOU: an attacker who can replace the file between check() and
    // execve() can still win that race. The allowlisted scripts are not writable
    // by the agent; openat2/fexecve is the future fix.
    [[nodiscard]] PolicyVerdict check(const std::vector<std::string>& argv) const;
    [[nodiscard]] const std::vector<Rule>& rules() const noexcept;

private:
    Policy() = default;

    std::vector<Rule> rules_;
    std::vector<std::filesystem::path> resolved_programs_;
    std::vector<std::string> deny_programs_;
    std::filesystem::path root_;
};

}  // namespace agenticpp
