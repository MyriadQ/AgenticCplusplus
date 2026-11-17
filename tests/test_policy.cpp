#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "agenticpp/policy.hpp"

namespace {

std::filesystem::path source_root() {
    return std::filesystem::path{AGENTICPP_SOURCE_DIR};
}

agenticpp::Result<agenticpp::Policy> load_policy() {
    return agenticpp::Policy::from_file(source_root() / "config/agent.json", source_root());
}

}  // namespace

TEST_CASE("Policy: exact allowlisted argv is allowed") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const agenticpp::Policy& policy = result.value();
    const auto verdict = policy.check({"scripts/kill-decoy.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Allow);
    REQUIRE(verdict.rule_id == "kill-decoy");
    REQUIRE(policy.rules().size() == 3);
}

TEST_CASE("Policy: shell command is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"sh", "-c", "rm -rf /"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("deny list") != std::string::npos);
}

TEST_CASE("Policy: path traversal is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/../../etc/passwd"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("traversal") != std::string::npos);
}

TEST_CASE("Policy: too many args is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/kill-decoy.sh", "extra"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.rule_id == "kill-decoy");
}

TEST_CASE("Policy: unknown program is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/unknown.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("not allowlisted") != std::string::npos);
}

TEST_CASE("Policy: empty argv is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("empty") != std::string::npos);
}

TEST_CASE("Policy: NUL byte is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    std::string program = "scripts/kill-decoy.sh";
    program.push_back('\0');

    const auto verdict = result.value().check({program});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("NUL") != std::string::npos);
}

TEST_CASE("Policy: program must use an explicit path") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"kill-decoy.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("explicit path") != std::string::npos);
}

TEST_CASE("Policy: missing config returns an error") {
    auto result =
        agenticpp::Policy::from_file(source_root() / "config/does-not-exist.json", source_root());

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code != 0);
}
