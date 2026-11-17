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
};

class Policy {
public:
    static Result<Policy> from_file(const std::filesystem::path& config_path,
                                    const std::filesystem::path& root);

    [[nodiscard]] PolicyVerdict check(const std::vector<std::string>& argv) const;
    [[nodiscard]] const std::vector<Rule>& rules() const noexcept;

private:
    Policy() = default;

    std::vector<Rule> rules_;
    std::vector<std::string> deny_programs_;
    std::filesystem::path root_;
};

}  // namespace agenticpp
