#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agenticpp/template_parser.hpp"

namespace agenticpp {

struct SkillConfig {
    std::filesystem::path project_root;
    std::filesystem::path log_path;
    std::filesystem::path run_dir;
    int default_port = 18080;
    std::size_t recent_log_bytes = 256 * 1024;
};

struct Evidence {
    std::string source;
    std::string detail;
};

struct SkillResult {
    bool conclusive = false;
    std::string cause;
    double confidence = 0.0;
    std::vector<Evidence> evidence;
    std::vector<std::string> suggested_argv;
};

struct SkillContext {
    std::string_view line;
    const ParsedLine& parsed;
};

class Skill {
public:
    virtual ~Skill() = default;
    virtual SkillResult probe(const SkillContext& context) const = 0;
};

// Runs the deterministic read-only checks that are cheap enough to execute
// before asking the model. The first high-confidence conclusive result supplies
// a proposed remediation; all evidence is still carried into the LLM prompt.
class SkillRegistry {
public:
    explicit SkillRegistry(SkillConfig config);
    explicit SkillRegistry(std::vector<std::unique_ptr<Skill>> skills);

    [[nodiscard]] SkillResult probe(const SkillContext& context) const;

private:
    SkillConfig config_;
    std::vector<std::unique_ptr<Skill>> skills_;
};

}  // namespace agenticpp
